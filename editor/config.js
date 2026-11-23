// SMC-Mixer configuration: the binary format of fw/smcpad/examples/mixer/mixer_config.h (version
// 4), shared by the editor and its simulated device. A configuration as an object:
//   { mode: 'daw'|'cc', fader_takeover: 'jump'|'pickup', fader_led, follow_incoming,
//     remember_values, accel, accel_ms_x3, accel_ms_x2, pages, channel_base[8] (per page: strip 1's MIDI channel - 1),
//     CC mode's profile: name, cc_fader, cc_fader_shift, cc_mute, cc_encoder (a CC number, or null
//       for none where allowed), solo_mutes, cc_solo (used when solo_mutes is off),
//       fader_special[4] (Shift + Mute / Solo / Rec / Track Stop: the fader's CC, 'pitchbend',
//       'pressure' or null),
//       focus_rec[8], focus_stop[8] (per encoder: the CC it sends while the group is on, or null),
//     mappings: [{ mode: 'cc'|'daw', control, page (0 = all), shift, type, channel (1..16), number,
//                  flags, min, max }] }
// A template (editor/templates/*.json) is CC mode's part of it: the profile, pages, channel bases,
// follow_incoming and the CC-mode mappings, plus who made it for what.
'use strict';

const CFG = {
  MAGIC: 'SMXC', VERSION: 4, HEADER: 84, ENTRY: 10, MAX_ENTRIES: 238, MAX_BYTES: 2464,
  NONE: 0x80, NAME_LEN: 28, LAYER_SHIFT: 0x10, LAYER_DAW: 0x20,
  TYPES: ['disabled', 'cc', 'note', 'program', 'pitchbend', 'pressure', 'base'],
  MODES: ['daw', 'cc'],
  TAKEOVERS: ['jump', 'pickup'],
  // flags
  BUTTON_MASK: 0x03, MOMENTARY: 0x00, TOGGLE: 0x01, TRIGGER: 0x02,
  TAKEOVER_MASK: 0x0C, TAKEOVER_INHERIT: 0x00, TAKEOVER_JUMP: 0x04, TAKEOVER_PICKUP: 0x08,
  ACCEL_MASK: 0x30, ACCEL_INHERIT: 0x00, ACCEL_OFF: 0x10, ACCEL_ON: 0x20,
};

// controls: 0..7 encoders, 8..15 faders, 16..60 buttons (smcmixer_hw.h order)
const CONTROLS = (() => {
  const rows = ['Mute', 'Solo', 'Rec', 'Track Stop'];
  const bottom = ['Play', 'Pause', 'Record', 'Rewind', 'Fast-forward', '<<', '>>', 'Up', 'Down', 'Left', 'Right'];
  const list = [];
  for (let k = 0; k < 8; k++) list.push({ id: k, kind: 'encoder', name: `Encoder ${k + 1}`, short: `E${k + 1}`, strip: k });
  for (let f = 0; f < 8; f++) list.push({ id: 8 + f, kind: 'fader', name: `Fader ${f + 1}`, short: `F${f + 1}`, strip: f });
  for (let b = 0; b < 32; b++) {
    const s = Math.floor(b / 4), r = b % 4;
    list.push({ id: 16 + b, kind: 'button', name: `Strip ${s + 1} ${rows[r]}`, short: r === 3 ? 'Stop' : rows[r], strip: s, row: r });
  }
  bottom.forEach((n, i) => list.push({ id: 48 + i, kind: 'button', name: n, short: n, bottom: i }));
  list.push({ id: 59, kind: 'button', name: 'Bluetooth', short: 'BT', oval: 0 });
  list.push({ id: 60, kind: 'button', name: 'Shift', short: 'Shift', oval: 1, reserved: true });
  return list;
})();

// What a mode does with a control when nothing is mapped, for the editor's "inherited" display.
const DAW_NOTES = [94, 93, 95, 91, 92, 46, 47, 96, 97, 98, 99];
const ROWS = ['mute', 'solo', 'rec', 'stop'];

function reservedFor(id, shift) {
  if (id === 60) return 'Shift';
  if (shift && (id === 48 + 3 || id === 48 + 4)) return id === 51 ? 'page −' : 'page +';
  if (shift && (id === 48 + 9 || id === 48 + 10)) return id === 57 ? 'DAW mode' : 'CC mode';
  return null;
}

function baseDescription(cfg, id, page, shift, mode) {
  const c = CONTROLS[id];
  const tb = cfg.channel_base[(page || 1) - 1] || 0;
  const r = reservedFor(id, shift);
  if (r) return `${r} (reserved)`;
  if (mode === 'daw') {
    if (c.kind === 'encoder') return `DAW: CC ${16 + c.strip} relative (V-Pot)`;
    if (c.kind === 'fader') return `DAW: pitch bend ch ${c.strip + 1} + touch note ${104 + c.strip}`;
    if (c.row !== undefined) return `DAW: note ${[16, 8, 0, 24][c.row] + c.strip}`;
    if (c.bottom !== undefined) return `DAW: note ${DAW_NOTES[c.bottom]}`;
    return 'nothing';
  }
  const tr = tb + (c.strip || 0) + 1;
  const name = cfg.name ? `${cfg.name}: ` : '';
  if (c.kind === 'encoder') {
    const fr = cfg.focus_rec[c.strip], fs = cfg.focus_stop[c.strip];
    const focus = [fr === null ? null : `Rec focus: CC ${fr}`, fs === null ? null : `Track Stop focus: CC ${fs}`]
      .filter(Boolean).join(', ');
    const own = cfg.cc_encoder === null ? 'nothing' : `CC ${cfg.cc_encoder} ch ${tr}`;
    return `${name}${own}${focus ? ` (${focus}, on the focused strip's channel)` : ''}`;
  }
  if (c.kind === 'fader') {
    if (shift && cfg.cc_fader_shift !== null) return `${name}CC ${cfg.cc_fader_shift} ch ${tr}`;
    return `${name}CC ${cfg.cc_fader} ch ${tr}${cfg.fader_special.some(v => v !== null) ? ' (Shift + strip button: special modes)' : ''}`;
  }
  if (c.row !== undefined) {
    if (shift && cfg.fader_special[c.row] !== null) {
      return `${name}fader ${c.strip + 1} special mode: ${specialText(cfg.fader_special[c.row])} (again: back)`;
    }
    const what = [
      cfg.cc_mute === null ? null : `mute ch ${tr} (CC ${cfg.cc_mute})`,
      soloText(cfg, tr),
      focusText(cfg.focus_rec, tr), focusText(cfg.focus_stop, tr)][c.row];
    return what ? name + what : 'nothing';
  }
  if (c.bottom !== undefined) return `note ${DAW_NOTES[c.bottom]} (as the stock DAW mode)`;
  return 'nothing';
}

// The same, short, for the drawing.
function baseShort(cfg, id, page, shift, mode) {
  const c = CONTROLS[id];
  const tb = cfg.channel_base[(page || 1) - 1] || 0;
  const tr = tb + (c.strip || 0) + 1;
  const r = reservedFor(id, shift);
  if (r) return r;
  if (mode === 'daw') {
    if (c.kind === 'encoder') return `V-Pot CC${16 + c.strip}`;
    if (c.kind === 'fader') return `PB ch${c.strip + 1}`;
    if (c.row !== undefined) return `note ${[16, 8, 0, 24][c.row] + c.strip}`;
    if (c.bottom !== undefined) return `note ${DAW_NOTES[c.bottom]}`;
    return '—';
  }
  if (c.kind === 'encoder') return cfg.cc_encoder === null ? '—' : `CC${cfg.cc_encoder} ch${tr}`;
  if (c.kind === 'fader') return shift && cfg.cc_fader_shift !== null ? `CC${cfg.cc_fader_shift} ch${tr}` : `CC${cfg.cc_fader} ch${tr}`;
  if (c.row !== undefined) {
    if (shift && cfg.fader_special[c.row] !== null) return `→ ${specialShort(cfg.fader_special[c.row])}`;
    return [cfg.cc_mute === null ? '—' : `mute ch${tr}`, soloText(cfg, tr) ? `solo ch${tr}` : '—',
            focusUsed(cfg.focus_rec) ? `focus ch${tr}` : '—', focusUsed(cfg.focus_stop) ? `focus ch${tr}` : '—'][c.row];
  }
  if (c.bottom !== undefined) return `note ${DAW_NOTES[c.bottom]}`;
  return '—';
}

// What Solo does in CC mode: mute the others (wins), send the device's solo CC, or nothing.
function soloText(cfg, tr) {
  if (cfg.solo_mutes) return cfg.cc_mute !== null ? `solo ch ${tr} (mutes the other channels)` : null;
  return cfg.cc_solo !== null ? `solo ch ${tr} (CC ${cfg.cc_solo})` : null;
}

// Fader special modes (Shift + a strip button): a CC, 'pitchbend', 'pressure' or null.
const SPECIAL_BYTES = { pitchbend: 0x81, pressure: 0x82 };
const specialByte = v => (v === null || v === undefined ? CFG.NONE : SPECIAL_BYTES[v] || (v & 127));
const specialVal = b => (b === 0x81 ? 'pitchbend' : b === 0x82 ? 'pressure' : b & CFG.NONE ? null : b);
const specialText = v => (v === 'pitchbend' ? 'pitch bend' : v === 'pressure' ? 'channel pressure' : `CC ${v}${v === 1 ? ' (mod wheel)' : v === 10 ? ' (pan)' : ''}`);
const specialShort = v => (v === 'pitchbend' ? 'bend' : v === 'pressure' ? 'press' : `CC${v}`);

// Focus groups (Rec / Track Stop): per encoder a CC or null.
const focusUsed = g => g.some(v => v !== null);
const focusOverlap = cfg => cfg.focus_rec.some((v, k) => v !== null && cfg.focus_stop[k] !== null);
function focusText(g, tr) {
  const used = g.map((v, k) => (v === null ? null : `${k + 1}`)).filter(Boolean);
  return used.length ? `encoders ${used.join(', ')} focus on ch ${tr}` : null;
}

function crc32(bytes) {
  let crc = 0xFFFFFFFF;
  for (const b of bytes) {
    crc ^= b;
    for (let k = 0; k < 8; k++) crc = (crc >>> 1) ^ (0xEDB88320 & -(crc & 1));
  }
  return (crc ^ 0xFFFFFFFF) >>> 0;
}

// CC mode's profile and what a template carries
const PROFILE_FIELDS = ['name', 'cc_fader', 'cc_fader_shift', 'cc_mute', 'cc_encoder', 'solo_mutes', 'cc_solo', 'fader_special',
                        'focus_rec', 'focus_stop'];
const TEMPLATE_SETTINGS = ['pages', 'channel_base', 'follow_incoming', 'cc_fader', 'cc_fader_shift', 'cc_mute', 'cc_encoder',
                           'solo_mutes', 'cc_solo', 'fader_special', 'focus_rec', 'focus_stop'];
const N = null;

function defaultConfig() {
  // as fw/smcpad/examples/mixer/mixer_config.c defaults(): DAW mode, CC mode = "OP-XY - blen2r"
  return {
    mode: 'daw', fader_takeover: 'pickup', fader_led: true, follow_incoming: true,
    remember_values: true, accel: true, accel_ms_x3: 6, accel_ms_x2: 12, pages: 2,
    channel_base: [0, 8, 0, 8, 0, 8, 0, 8],
    name: 'OP-XY - blen2r', cc_fader: 7, cc_fader_shift: 10, cc_mute: 9, cc_encoder: 12,
    solo_mutes: true, cc_solo: null, fader_special: [1, 'pitchbend', 'pressure', 10],
    focus_rec: [12, 13, 14, 15, N, N, N, N], focus_stop: [N, N, N, N, 12, 13, 14, 15],
    mappings: [],
  };
}

const ccByte = v => (v === null || v === undefined ? CFG.NONE : v & 127);
const ccVal = b => (b & CFG.NONE ? null : b);

function encodeConfig(cfg) {
  const n = cfg.mappings.length;
  if (n > CFG.MAX_ENTRIES) throw new Error(`too many mappings (${n} > ${CFG.MAX_ENTRIES})`);
  const buf = new Uint8Array(CFG.HEADER + n * CFG.ENTRY);
  const dv = new DataView(buf.buffer);
  for (let i = 0; i < 4; i++) buf[i] = CFG.MAGIC.charCodeAt(i);
  dv.setUint16(4, CFG.VERSION, true);
  dv.setUint16(6, n, true);
  buf[8] = CFG.MODES.indexOf(cfg.mode);
  buf[9] = CFG.TAKEOVERS.indexOf(cfg.fader_takeover);
  buf[10] = cfg.fader_led ? 1 : 0;
  buf[11] = cfg.follow_incoming ? 1 : 0;
  buf[12] = cfg.remember_values ? 1 : 0;
  buf[13] = cfg.accel ? 1 : 0;
  buf[14] = cfg.accel_ms_x3;
  buf[15] = cfg.accel_ms_x2;
  buf[16] = cfg.pages;
  for (let p = 0; p < 8; p++) buf[17 + p] = cfg.channel_base[p] & 15;
  buf[25] = ccByte(cfg.cc_fader);
  buf[26] = ccByte(cfg.cc_fader_shift);
  buf[27] = ccByte(cfg.cc_mute);
  buf[28] = ccByte(cfg.cc_encoder);
  buf[29] = ccByte(cfg.cc_solo);
  buf[30] = (cfg.solo_mutes ? 1 : 0) | (cfg.cc_solo !== null ? 4 : 0);
  const name = (cfg.name || '').replace(/[^\x20-\x7e]/g, '?').slice(0, CFG.NAME_LEN);
  for (let i = 0; i < name.length; i++) buf[32 + i] = name.charCodeAt(i);
  for (let k = 0; k < 8; k++) { buf[60 + k] = ccByte(cfg.focus_rec[k]); buf[68 + k] = ccByte(cfg.focus_stop[k]); }
  for (let k = 0; k < 4; k++) buf[76 + k] = specialByte(cfg.fader_special[k]);
  cfg.mappings.forEach((m, i) => {
    const o = CFG.HEADER + i * CFG.ENTRY;
    buf[o] = m.control;
    buf[o + 1] = (m.page & 15) | (m.shift ? CFG.LAYER_SHIFT : 0) | (m.mode === 'daw' ? CFG.LAYER_DAW : 0);
    buf[o + 2] = CFG.TYPES.indexOf(m.type);
    buf[o + 3] = (m.channel - 1) & 15;
    buf[o + 4] = m.number & 127;
    buf[o + 5] = m.flags & 0xFF;
    buf[o + 6] = m.min & 127;
    buf[o + 7] = m.max & 127;
  });
  dv.setUint32(80, 0, true);
  dv.setUint32(80, crc32(buf), true);
  return buf;
}

function decodeConfig(buf) {
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  const magic = String.fromCharCode(...buf.slice(0, 4));
  if (magic !== CFG.MAGIC) throw new Error('not an SMC-Mixer configuration');
  const version = dv.getUint16(4, true);
  if (version !== CFG.VERSION) throw new Error(`configuration version ${version}: update the mixer's firmware`);
  const n = dv.getUint16(6, true);
  const nameBytes = Array.from(buf.slice(32, 32 + CFG.NAME_LEN));
  const end = nameBytes.indexOf(0);
  const cfg = {
    mode: CFG.MODES[buf[8]] || 'daw', fader_takeover: CFG.TAKEOVERS[buf[9]] || 'pickup',
    fader_led: !!buf[10], follow_incoming: !!buf[11], remember_values: !!buf[12], accel: !!buf[13],
    accel_ms_x3: buf[14], accel_ms_x2: buf[15], pages: buf[16],
    channel_base: Array.from(buf.slice(17, 25)),
    name: String.fromCharCode(...(end < 0 ? nameBytes : nameBytes.slice(0, end))),
    cc_fader: ccVal(buf[25]), cc_fader_shift: ccVal(buf[26]), cc_mute: ccVal(buf[27]), cc_encoder: ccVal(buf[28]),
    solo_mutes: !!(buf[30] & 1), cc_solo: buf[30] & 4 ? ccVal(buf[29]) : null,
    fader_special: Array.from(buf.slice(76, 80), specialVal),
    focus_rec: Array.from(buf.slice(60, 68), ccVal), focus_stop: Array.from(buf.slice(68, 76), ccVal),
    mappings: [],
  };
  for (let i = 0; i < n; i++) {
    const o = CFG.HEADER + i * CFG.ENTRY;
    cfg.mappings.push({
      mode: buf[o + 1] & CFG.LAYER_DAW ? 'daw' : 'cc',
      control: buf[o], page: buf[o + 1] & 15, shift: !!(buf[o + 1] & CFG.LAYER_SHIFT), type: CFG.TYPES[buf[o + 2]],
      channel: buf[o + 3] + 1, number: buf[o + 4], flags: buf[o + 5], min: buf[o + 6], max: buf[o + 7],
    });
  }
  return cfg;
}

// Profile fields of older exports and templates: cc_knob -> cc_encoder; special_modes ->
// fader_special; track_base -> channel_base; cc_focus (one CC: Rec =
// encoders 1-4 on it and the 3 next, Track Stop = encoders 5-8 on the same) -> the two focus groups.
function normalizeSettings(o) {
  const out = Object.assign({}, o);
  if ('cc_knob' in out) {
    if (!('cc_encoder' in out)) out.cc_encoder = out.cc_knob;
    delete out.cc_knob;
  }
  if ('focus_select' in out) {               // the first version 3 editor's name for focus_stop
    if (!('focus_stop' in out)) out.focus_stop = out.focus_select;
    delete out.focus_select;
  }
  if ('track_base' in out) {                 // before: the same, called track_base
    if (!('channel_base' in out)) out.channel_base = out.track_base;
    delete out.track_base;
  }
  if ('special_modes' in out) {              // up to version 3: one switch for a fixed set
    if (!('fader_special' in out)) out.fader_special = out.special_modes ? [1, 'pitchbend', 'pressure', 10] : [null, null, null, null];
    delete out.special_modes;
  }
  if ('cc_focus' in out) {
    const f = out.cc_focus;
    if (!('focus_rec' in out)) out.focus_rec = [0, 1, 2, 3, 4, 5, 6, 7].map(k => (f === null || k > 3 ? null : (f + k) & 127));
    if (!('focus_stop' in out)) out.focus_stop = [0, 1, 2, 3, 4, 5, 6, 7].map(k => (f === null || k < 4 ? null : (f + k - 4) & 127));
    delete out.cc_focus;
  }
  return out;
}

// A configuration from an imported file: older exports (base: 'opxy' / 'daw', no mode on mappings,
// cc_knob / cc_focus) are brought up to date.
function normalizeConfig(obj) {
  const cfg = Object.assign(defaultConfig(), normalizeSettings(obj));
  if (obj.base !== undefined && obj.mode === undefined) cfg.mode = obj.base === 'daw' ? 'daw' : 'cc';
  delete cfg.base;
  cfg.mappings = (cfg.mappings || []).map(m => Object.assign({ mode: obj.base === 'daw' ? 'daw' : 'cc' }, m));
  return cfg;
}

// ---- templates

function templateFromConfig(cfg, meta) {
  const settings = {};
  for (const k of TEMPLATE_SETTINGS) settings[k] = Array.isArray(cfg[k]) ? cfg[k].slice() : cfg[k];
  delete settings.name;
  return {
    format: 2, name: meta.name, device: meta.device, author: meta.author, description: meta.description || '',
    settings,
    mappings: cfg.mappings.filter(m => m.mode === 'cc').map(m => {
      const { mode, ...rest } = m;
      return rest;
    }),
  };
}

// Problems with a template (empty: fine). Names carry their author: "OP-XY - blen2r".
function checkTemplate(t) {
  const out = [];
  for (const k of ['name', 'device', 'author']) if (!t[k] || typeof t[k] !== 'string') out.push(`missing "${k}"`);
  if (t.name && t.author && !t.name.includes(` - ${t.author}`)) out.push(`the name must contain " - ${t.author}" ("OP-XY - ${t.author}", "OP-XY - ${t.author} alt 1")`);
  if (t.name && t.name.length > CFG.NAME_LEN) out.push(`the name is longer than ${CFG.NAME_LEN} characters`);
  if (!t.settings || typeof t.settings !== 'object') out.push('missing "settings"');
  if (!Array.isArray(t.mappings)) out.push('missing "mappings"');
  else {
    try {
      const cfg = applyTemplate(defaultConfig(), t);
      decodeConfig(encodeConfig(cfg));
    } catch (e) { out.push(e.message); }
  }
  return out;
}

// CC mode's part of cfg replaced by the template's; DAW mode's mappings and the personal settings
// (takeover, LED, acceleration, remembering) stay.
function applyTemplate(cfg, t) {
  const out = JSON.parse(JSON.stringify(cfg));
  const settings = Object.assign({ cc_solo: null }, normalizeSettings(t.settings || {}));
  for (const k of TEMPLATE_SETTINGS) if (settings[k] !== undefined) out[k] = JSON.parse(JSON.stringify(settings[k]));
  out.name = t.name;
  out.mappings = out.mappings.filter(m => m.mode !== 'cc')
    .concat((t.mappings || []).map(m => Object.assign({ mode: 'cc' }, m, { mode: 'cc' })));
  return out;
}

// The mapping that applies to a control on a page / layer, as the firmware resolves it.
// Returns { mapping, from: 'here'|'page'|'all+shift'|'all' } or null (base).
function resolveMapping(cfg, control, page, shift, mode) {
  const order = [[page, true], [page, false], [0, true], [0, false]];
  for (const [pg, sh] of order) {
    if (sh && !shift) continue;
    if (pg && !page) continue;
    const ms = cfg.mappings.filter(m => m.mode === mode && m.control === control && m.page === pg && m.shift === sh);
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
                     pack7, unpack7, baseDescription, baseShort, reservedFor, normalizeConfig,
                     templateFromConfig, checkTemplate, applyTemplate, normalizeSettings, focusUsed, focusOverlap,
                     PROFILE_FIELDS, TEMPLATE_SETTINGS };
}
