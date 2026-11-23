// SMC-Mixer configuration: the binary format of fw/smcpad/examples/mixer/mixer_config.h,
// shared by the editor and its simulated device. A configuration as an object:
//   { base: 'opxy'|'daw', fader_takeover: 'jump'|'pickup', fader_led, follow_incoming,
//     remember_values, accel, accel_ms_x3, accel_ms_x2, pages, track_base[8],
//     mappings: [{ control, page (0 = all), shift, type, channel (1..16), number, flags, min, max }] }
'use strict';

const CFG = {
  MAGIC: 'SMXC', VERSION: 1, HEADER: 32, ENTRY: 10, MAX_ENTRIES: 240,
  TYPES: ['disabled', 'cc', 'note', 'program', 'pitchbend', 'pressure', 'base'],
  BASES: ['daw', 'opxy'],
  TAKEOVERS: ['jump', 'pickup'],
  // flags
  BUTTON_MASK: 0x03, MOMENTARY: 0x00, TOGGLE: 0x01, TRIGGER: 0x02,
  TAKEOVER_MASK: 0x0C, TAKEOVER_INHERIT: 0x00, TAKEOVER_JUMP: 0x04, TAKEOVER_PICKUP: 0x08,
  ACCEL_MASK: 0x30, ACCEL_INHERIT: 0x00, ACCEL_OFF: 0x10, ACCEL_ON: 0x20,
};

// controls: 0..7 knobs, 8..15 faders, 16..60 buttons (smcmixer_hw.h order)
const CONTROLS = (() => {
  const rows = ['Mute', 'Solo', 'Rec', 'Select'];
  const bottom = ['Play', 'Pause', 'Record', 'Rewind', 'Fast-forward', '<<', '>>', 'Up', 'Down', 'Left', 'Right'];
  const list = [];
  for (let k = 0; k < 8; k++) list.push({ id: k, kind: 'knob', name: `Knob ${k + 1}`, short: `K${k + 1}`, strip: k });
  for (let f = 0; f < 8; f++) list.push({ id: 8 + f, kind: 'fader', name: `Fader ${f + 1}`, short: `F${f + 1}`, strip: f });
  for (let b = 0; b < 32; b++) {
    const s = Math.floor(b / 4), r = b % 4;
    list.push({ id: 16 + b, kind: 'button', name: `Strip ${s + 1} ${rows[r]}`, short: rows[r], strip: s, row: r });
  }
  bottom.forEach((n, i) => list.push({ id: 48 + i, kind: 'button', name: n, short: n, bottom: i }));
  list.push({ id: 59, kind: 'button', name: 'Bluetooth', short: 'BT', oval: 0 });
  list.push({ id: 60, kind: 'button', name: 'Shift', short: 'Shift', oval: 1, reserved: true });
  return list;
})();

// What the base profile does with a control, for the editor's "inherited" display.
function baseDescription(cfg, id, page, shift) {
  const c = CONTROLS[id];
  const tb = cfg.track_base[(page || 1) - 1] || 0;
  const DAW_NOTES = [94, 93, 95, 91, 92, 46, 47, 96, 97, 98, 99];
  if (id === 60) return 'Shift (reserved)';
  if (cfg.base === 'daw') {
    if (c.kind === 'knob') return `DAW: CC ${16 + c.strip} relative (V-Pot)`;
    if (c.kind === 'fader') return `DAW: pitch bend ch ${c.strip + 1} + touch note ${104 + c.strip}`;
    if (c.row !== undefined) return `DAW: note ${[16, 8, 0, 24][c.row] + c.strip}`;
    if (c.bottom !== undefined) return `DAW: note ${DAW_NOTES[c.bottom]}`;
    return 'nothing';
  }
  const tr = tb + (c.strip || 0) + 1;
  if (c.kind === 'knob') return `OP-XY: CC 12 ch ${tr} (Rec / Select focus: CC 12-15 of the focused track)`;
  if (c.kind === 'fader') return shift ? `OP-XY: pan, CC 10 ch ${tr}` : `OP-XY: volume, CC 7 ch ${tr} (Shift + strip button: special modes)`;
  if (c.row !== undefined) {
    const what = [`mute track ${tr} (CC 9)`, `solo track ${tr} (mutes the others)`,
                  `knobs 1-4 focus on track ${tr}`, `knobs 5-8 focus on track ${tr}`][c.row];
    const mode = ['mod wheel', 'pitch bend', 'channel pressure', 'pan'][c.row];
    return shift ? `OP-XY: fader ${c.strip + 1} special mode: ${mode}` : `OP-XY: ${what}`;
  }
  if (c.bottom !== undefined) {
    if (shift && (c.bottom === 3 || c.bottom === 4)) return `page ${c.bottom === 3 ? 'down' : 'up'} (reserved)`;
    return `note ${DAW_NOTES[c.bottom]} (as the stock DAW mode)`;
  }
  return 'nothing';
}

// The same, short, for the drawing.
function baseShort(cfg, id, page, shift) {
  const c = CONTROLS[id];
  const tb = cfg.track_base[(page || 1) - 1] || 0;
  const tr = tb + (c.strip || 0) + 1;
  const DAW_NOTES = [94, 93, 95, 91, 92, 46, 47, 96, 97, 98, 99];
  if (id === 60) return 'reserved';
  if (cfg.base === 'daw') {
    if (c.kind === 'knob') return `V-Pot CC${16 + c.strip}`;
    if (c.kind === 'fader') return `PB ch${c.strip + 1}`;
    if (c.row !== undefined) return `note ${[16, 8, 0, 24][c.row] + c.strip}`;
    if (c.bottom !== undefined) return `note ${DAW_NOTES[c.bottom]}`;
    return '—';
  }
  if (c.kind === 'knob') return `CC12 ch${tr}`;
  if (c.kind === 'fader') return shift ? `pan ch${tr}` : `vol ch${tr}`;
  if (c.row !== undefined) {
    if (shift) return ['→ mod', '→ bend', '→ press', '→ pan'][c.row];
    return [`mute T${tr}`, `solo T${tr}`, `focus A T${tr}`, `focus B T${tr}`][c.row];
  }
  if (c.bottom !== undefined) {
    if (shift && (c.bottom === 3 || c.bottom === 4)) return c.bottom === 3 ? 'page −' : 'page +';
    return `note ${DAW_NOTES[c.bottom]}`;
  }
  return '—';
}

function crc32(bytes) {
  let crc = 0xFFFFFFFF;
  for (const b of bytes) {
    crc ^= b;
    for (let k = 0; k < 8; k++) crc = (crc >>> 1) ^ (0xEDB88320 & -(crc & 1));
  }
  return (crc ^ 0xFFFFFFFF) >>> 0;
}

function defaultConfig() {
  return {
    base: 'opxy', fader_takeover: 'pickup', fader_led: true, follow_incoming: true,
    remember_values: true, accel: true, accel_ms_x3: 6, accel_ms_x2: 12, pages: 2,
    track_base: [0, 8, 0, 8, 0, 8, 0, 8], mappings: [],
  };
}

function encodeConfig(cfg) {
  const n = cfg.mappings.length;
  if (n > CFG.MAX_ENTRIES) throw new Error(`too many mappings (${n} > ${CFG.MAX_ENTRIES})`);
  const buf = new Uint8Array(CFG.HEADER + n * CFG.ENTRY);
  const dv = new DataView(buf.buffer);
  for (let i = 0; i < 4; i++) buf[i] = CFG.MAGIC.charCodeAt(i);
  dv.setUint16(4, CFG.VERSION, true);
  dv.setUint16(6, n, true);
  buf[8] = CFG.BASES.indexOf(cfg.base);
  buf[9] = CFG.TAKEOVERS.indexOf(cfg.fader_takeover);
  buf[10] = cfg.fader_led ? 1 : 0;
  buf[11] = cfg.follow_incoming ? 1 : 0;
  buf[12] = cfg.remember_values ? 1 : 0;
  buf[13] = cfg.accel ? 1 : 0;
  buf[14] = cfg.accel_ms_x3;
  buf[15] = cfg.accel_ms_x2;
  buf[16] = cfg.pages;
  for (let p = 0; p < 8; p++) buf[17 + p] = cfg.track_base[p] & 15;
  cfg.mappings.forEach((m, i) => {
    const o = CFG.HEADER + i * CFG.ENTRY;
    buf[o] = m.control;
    buf[o + 1] = (m.page & 15) | (m.shift ? 16 : 0);
    buf[o + 2] = CFG.TYPES.indexOf(m.type);
    buf[o + 3] = (m.channel - 1) & 15;
    buf[o + 4] = m.number & 127;
    buf[o + 5] = m.flags & 0xFF;
    buf[o + 6] = m.min & 127;
    buf[o + 7] = m.max & 127;
  });
  dv.setUint32(28, 0, true);
  dv.setUint32(28, crc32(buf), true);
  return buf;
}

function decodeConfig(buf) {
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  const magic = String.fromCharCode(...buf.slice(0, 4));
  if (magic !== CFG.MAGIC) throw new Error('not an SMC-Mixer configuration');
  const n = dv.getUint16(6, true);
  const cfg = {
    base: CFG.BASES[buf[8]] || 'opxy', fader_takeover: CFG.TAKEOVERS[buf[9]] || 'pickup',
    fader_led: !!buf[10], follow_incoming: !!buf[11], remember_values: !!buf[12], accel: !!buf[13],
    accel_ms_x3: buf[14], accel_ms_x2: buf[15], pages: buf[16],
    track_base: Array.from(buf.slice(17, 25)), mappings: [],
  };
  for (let i = 0; i < n; i++) {
    const o = CFG.HEADER + i * CFG.ENTRY;
    cfg.mappings.push({
      control: buf[o], page: buf[o + 1] & 15, shift: !!(buf[o + 1] & 16), type: CFG.TYPES[buf[o + 2]],
      channel: buf[o + 3] + 1, number: buf[o + 4], flags: buf[o + 5], min: buf[o + 6], max: buf[o + 7],
    });
  }
  return cfg;
}

// The mapping that applies to a control on a page / layer, as the firmware resolves it.
// Returns { mapping, from: 'here'|'page'|'all+shift'|'all' } or null (base).
function resolveMapping(cfg, control, page, shift) {
  const order = [[page, true], [page, false], [0, true], [0, false]];
  for (const [pg, sh] of order) {
    if (sh && !shift) continue;
    if (pg && !page) continue;
    const ms = cfg.mappings.filter(m => m.control === control && m.page === pg && m.shift === sh);
    if (ms.length) {
      const m = ms[ms.length - 1];
      const from = pg === page && sh === shift ? 'here' : `${pg ? 'page ' + pg : 'all pages'}${sh ? ' + Shift' : ''}`;
      return m.type === 'base' ? { mapping: null, base: true, from } : { mapping: m, from };
    }
  }
  return null;
}

// 8-to-7 packing of the editor protocol: a byte with the top bits of up to 7 bytes, then them.
function pack7(data) {
  const out = [];
  for (let i = 0; i < data.length; i += 7) {
    const g = data.slice(i, i + 7);
    let msb = 0;
    g.forEach((b, k) => { msb |= ((b >> 7) & 1) << k; });
    out.push(msb, ...Array.from(g, b => b & 0x7F));
  }
  return out;
}

function unpack7(data) {
  const out = [];
  for (let i = 0; i < data.length;) {
    const msb = data[i++];
    for (let k = 0; k < 7 && i < data.length; k++, i++) out.push(data[i] | (((msb >> k) & 1) << 7));
  }
  return new Uint8Array(out);
}

if (typeof module !== 'undefined') {
  module.exports = { CFG, CONTROLS, crc32, defaultConfig, encodeConfig, decodeConfig, resolveMapping,
                     pack7, unpack7, baseDescription, baseShort };
}
