// The connection to the mixer: WebMIDI to the real one (USB, "SMC-Mixer open fw"), or a
// simulated device speaking the same protocol, for working without the hardware.
// Protocol: docs/mixer-config.md ("Editor protocol"), fw/smcpad/examples/mixer/mixer_config.c.
'use strict';

class MixerLink {
  constructor() {
    this.seq = 0;
    this.pending = new Map();       // seq -> { resolve, reject, timer }
    this.onEvent = null;            // (control, value, page, shift)
    this.onMidi = null;             // (bytes) every non-SysEx message from the mixer
    this.name = '';
  }

  // ---- transport (subclasses: send(bytes))
  handle(bytes) {
    if (bytes[0] === 0xF0 && bytes[1] === 0x7D && bytes[2] === 0x4D) {
      const cmd = bytes[3], seq = bytes[4];
      if (cmd === 0x7F) {
        const p = this.pending.get(seq);
        if (p) {
          clearTimeout(p.timer);
          this.pending.delete(seq);
          p.resolve({ status: bytes[5], data: unpack7(bytes.slice(6, bytes.length - 1)) });
        }
      } else if (cmd === 0x10 && this.onEvent) {
        const d = unpack7(bytes.slice(5, bytes.length - 1));
        this.onEvent(d[0], (d[1] << 8) | d[2], d[3], d[4]);
      }
      return;
    }
    if (bytes[0] !== 0xF0 && this.onMidi) this.onMidi(bytes);
  }

  command(cmd, data = [], timeout = 2000) {
    this.seq = (this.seq + 1) & 0x7F;
    const seq = this.seq;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => { this.pending.delete(seq); reject(new Error(`no answer to command ${cmd}`)); }, timeout);
      this.pending.set(seq, { resolve, reject, timer });
      this.send([0xF0, 0x7D, 0x4D, cmd, seq, ...pack7(Uint8Array.from(data)), 0xF7]);
    });
  }

  // ---- protocol
  async hello() {
    const r = await this.command(0x01);
    const d = r.data, u16 = i => d[i] | (d[i + 1] << 8);
    const h = { fw: u16(0), configVersion: u16(2), maxBytes: u16(4), controls: d[6], pages: d[7], page: d[8], length: u16(9),
                mode: d.length > 11 ? d[11] : undefined };
    if (h.configVersion !== CFG.VERSION) throw new Error(`the mixer's firmware speaks configuration version ${h.configVersion}, this editor ${CFG.VERSION}: update the firmware (host/mixer_update.sh)`);
    return h;
  }

  async readConfig() {
    const h = await this.hello();
    const blob = new Uint8Array(h.length);
    for (let off = 0; off < h.length; off += 128) {
      const n = Math.min(128, h.length - off);
      const r = await this.command(0x02, [off & 0xFF, off >> 8, n]);
      if (r.status) throw new Error(`read failed (status ${r.status})`);
      blob.set(r.data.slice(0, n), off);
    }
    return decodeConfig(blob);
  }

  async writeConfig(cfg) {
    const blob = encodeConfig(cfg);
    for (let off = 0; off < blob.length; off += 96) {
      const r = await this.command(0x03, [off & 0xFF, off >> 8, ...blob.slice(off, off + 96)]);
      if (r.status) throw new Error(`write failed at ${off} (status ${r.status})`);
    }
    const r = await this.command(0x04, [blob.length & 0xFF, blob.length >> 8], 5000);
    if (r.status) throw new Error(['', 'bad request', 'the mixer refused the configuration', 'storing it failed'][r.status] || `status ${r.status}`);
  }

  async factoryReset() {
    const r = await this.command(0x06, [], 5000);
    if (r.status) throw new Error(`factory reset failed (status ${r.status})`);
  }

  async events(on) { await this.command(0x10, [on ? 1 : 0]); }
}

class WebMidiLink extends MixerLink {
  static async open() {
    if (!navigator.requestMIDIAccess) throw new Error('this browser has no Web MIDI (use Chrome, Edge or Chromium)');
    const access = await navigator.requestMIDIAccess({ sysex: true });
    const find = ports => [...ports.values()].find(p => /SMC-Mixer open fw/.test(p.name));
    const input = find(access.inputs), output = find(access.outputs);
    if (!input || !output) throw new Error('no "SMC-Mixer open fw" on USB: is the mixer plugged in, with our firmware?');
    const link = new WebMidiLink();
    link.input = input;
    link.output = output;
    link.name = `${input.name} (USB)`;
    input.onmidimessage = e => link.handle(Array.from(e.data));
    return link;
  }

  send(bytes) { this.output.send(bytes); }
  close() { this.input.onmidimessage = null; }
}

// A mixer in the browser: the same protocol, a configuration in memory, and a few moving
// controls so the monitor has something to show.
class SimulatedLink extends MixerLink {
  constructor() {
    super();
    this.name = 'simulated mixer';
    this.active = encodeConfig(this.loadSaved() || defaultConfig());
    this.staging = new Uint8Array(CFG.MAX_BYTES);
    this.staging.set(this.active);
    this.eventsOn = false;
  }

  loadSaved() {
    try { const c = localStorage.getItem('smc-mixer-sim'); return c ? normalizeConfig(JSON.parse(c)) : null; } catch (e) { return null; }
  }

  reply(seq, status, data = []) {
    setTimeout(() => this.handle([0xF0, 0x7D, 0x4D, 0x7F, seq, status, ...pack7(Uint8Array.from(data)), 0xF7]), 5);
  }

  send(bytes) {
    const cmd = bytes[3], seq = bytes[4], d = unpack7(bytes.slice(5, bytes.length - 1));
    const u16 = i => d[i] | (d[i + 1] << 8);
    switch (cmd) {
      case 0x01: {
        const l = this.active.length;
        this.reply(seq, 0, [3, 0, CFG.VERSION, 0, CFG.MAX_BYTES & 0xFF, CFG.MAX_BYTES >> 8, 61, 8, 1, l & 0xFF, l >> 8, this.active[8]]);
        break;
      }
      case 0x02: this.reply(seq, 0, Array.from(this.active.slice(u16(0), u16(0) + d[2]))); break;
      case 0x03: this.staging.set(d.slice(2), u16(0)); this.reply(seq, 0); break;
      case 0x04: {
        const blob = this.staging.slice(0, u16(0));
        try {
          const cfg = decodeConfig(blob);
          const check = encodeConfig(cfg);
          if (check.length !== blob.length || check.some((b, i) => b !== blob[i])) throw new Error('crc');
          this.active = blob;
          try { localStorage.setItem('smc-mixer-sim', JSON.stringify(cfg)); } catch (e) { /* private mode */ }
          this.reply(seq, 0);
        } catch (e) { this.reply(seq, 2); }
        break;
      }
      case 0x06:
        this.active = encodeConfig(defaultConfig());
        try { localStorage.removeItem('smc-mixer-sim'); } catch (e) { /* ignore */ }
        this.reply(seq, 0);
        break;
      case 0x10: this.eventsOn = !!d[0]; this.reply(seq, 0); break;
      default: this.reply(seq, 1);
    }
  }

  close() {}
}
