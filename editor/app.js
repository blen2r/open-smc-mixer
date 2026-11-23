// SMC-Mixer editor: the mixer drawing, the mapping editor, global settings, monitor.
'use strict';

const $ = s => document.querySelector(s);
const state = { link: null, cfg: null, saved: '', page: 0, shift: false, sel: null, faders: new Array(8).fill(0) };

function toast(msg, err) {
  const t = $('#toast');
  t.textContent = msg;
  t.className = 'toast show' + (err ? ' err' : '');
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => { t.className = 'toast'; }, err ? 6000 : 2500);
}

function markDirty() {
  const dirty = state.cfg && JSON.stringify(state.cfg) !== state.saved;
  $('#dirty').textContent = dirty ? '● unsaved changes' : '';
  $('#btnSave').disabled = !state.link || !dirty;
}

// ------------------------------------------------------------------ mapping text

function mappingText(m) {
  if (!m) return '';
  const ch = `ch ${m.channel}`;
  switch (m.type) {
    case 'disabled': return 'disabled';
    case 'cc': return `CC ${m.number} ${ch}`;
    case 'note': return `note ${m.number} ${ch}`;
    case 'program': return `PC ${m.number} ${ch}`;
    case 'pitchbend': return `pitch bend ${ch}`;
    case 'pressure': return `pressure ${ch}`;
    default: return m.type;
  }
}

function controlInfo(id) {
  const cfg = state.cfg;
  const r = resolveMapping(cfg, id, state.page, state.shift);
  if (CONTROLS[id].reserved) return { cls: 'reserved', text: 'reserved' };
  if (!r || r.base) return { cls: 'base', text: baseShort(cfg, id, state.page, state.shift), full: baseDescription(cfg, id, state.page, state.shift) };
  const cls = r.from === 'here' ? 'here' : 'inherited';
  return { cls: (r.mapping.type === 'disabled' ? 'disabled ' : '') + cls, text: mappingText(r.mapping), from: r.from };
}

// ------------------------------------------------------------------ drawing

function ctlEl(id, extraClass, inner) {
  const c = CONTROLS[id];
  const el = document.createElement('div');
  el.className = `ctl ${extraClass || ''}`;
  el.dataset.id = id;
  el.innerHTML = (inner || '') + `<div class="n">${c.short}</div><div class="v"></div>`;
  if (!c.reserved) el.onclick = () => select(id);
  return el;
}

function buildSurface() {
  const surface = $('#surface'), bottom = $('#bottom');
  surface.innerHTML = '';
  bottom.innerHTML = '';
  for (let s = 0; s < 8; s++) {
    const strip = document.createElement('div');
    strip.className = 'strip';
    strip.appendChild(ctlEl(s, 'knob'));
    const led = document.createElement('div');
    led.className = 'led';
    led.id = `led${s}`;
    strip.appendChild(led);
    for (let r = 0; r < 4; r++) strip.appendChild(ctlEl(16 + s * 4 + r));
    strip.appendChild(ctlEl(8 + s, 'fader', '<div class="track"><div class="cap" id="cap' + s + '"></div></div>'));
    surface.appendChild(strip);
  }
  const side = document.createElement('div');
  side.className = 'side';
  side.appendChild(ctlEl(59));
  side.appendChild(ctlEl(60, 'reserved'));
  surface.appendChild(side);
  for (let i = 0; i < 11; i++) bottom.appendChild(ctlEl(48 + i));
}

function refreshSurface() {
  if (!state.cfg) return;
  document.querySelectorAll('.ctl').forEach(el => {
    const id = +el.dataset.id;
    const info = controlInfo(id);
    el.classList.remove('here', 'inherited', 'base', 'disabled');
    info.cls.split(' ').forEach(c => c && el.classList.add(c));
    el.classList.toggle('sel', id === state.sel);
    el.querySelector('.v').textContent = info.text;
    el.title = `${CONTROLS[id].name}: ${info.full || info.text}${info.from && info.from !== 'here' ? ` (from ${info.from})` : ''}`;
  });
  const pages = $('#pages');
  pages.innerHTML = '';
  const mk = (label, p) => {
    const b = document.createElement('button');
    b.textContent = label;
    b.className = state.page === p ? 'on' : '';
    b.onclick = () => { state.page = p; refreshAll(); };
    pages.appendChild(b);
  };
  mk('All pages', 0);
  for (let p = 1; p <= state.cfg.pages; p++) mk(String(p), p);
  document.querySelectorAll('#layers button').forEach(b => b.classList.toggle('on', (b.dataset.shift === '1') === state.shift));
}

// ------------------------------------------------------------------ control editor

function select(id) {
  state.sel = id;
  refreshAll();
}

function findHere(id) {
  return state.cfg.mappings.find(m => m.control === id && m.page === state.page && m.shift === state.shift);
}

function setHere(id, m) {
  const ms = state.cfg.mappings.filter(x => !(x.control === id && x.page === state.page && x.shift === state.shift));
  if (m) ms.push(Object.assign({ control: id, page: state.page, shift: state.shift }, m));
  state.cfg.mappings = ms;
  refreshAll();
}

function field(label, html) {
  return `<div class="row"><label>${label}</label>${html}</div>`;
}

function renderEditor() {
  const p = $('#editPanel');
  if (!state.cfg || state.sel === null) {
    p.innerHTML = '<h2>Control</h2><p class="hint">Select a control on the mixer drawing.</p>';
    return;
  }
  const id = state.sel, c = CONTROLS[id];
  const here = findHere(id);
  const r = resolveMapping(state.cfg, id, state.page, state.shift);
  const layer = `${state.page ? 'page ' + state.page : 'all pages'}${state.shift ? ' + Shift' : ''}`;
  let src;
  if (here) src = `Mapped on <b>${layer}</b>.`;
  else if (r && !r.base) src = `Not mapped on ${layer}: inherits <b>${mappingText(r.mapping)}</b> from ${r.from}.`;
  else src = `Not mapped: the base profile does <b>${baseDescription(state.cfg, id, state.page, state.shift)}</b>.`;

  const m = here || { type: 'inherit', channel: 1, number: 0, flags: 0, min: 0, max: 127 };
  const types = ['inherit', 'disabled', 'cc', 'note', 'program', 'pitchbend', 'pressure', 'base'];
  const typeLabels = { inherit: 'inherit (no mapping here)', disabled: 'disabled (send nothing)', cc: 'CC', note: 'note',
    program: 'program change', pitchbend: 'pitch bend', pressure: 'channel pressure', base: 'base profile (explicitly)' };
  let html = `<h2>${c.name}</h2><div class="src">${src}</div>`;
  html += field('Type', `<select id="eType">${types.map(t => `<option value="${t}" ${m.type === t || (!here && t === 'inherit') ? 'selected' : ''}>${typeLabels[t]}</option>`).join('')}</select>`);
  const active = here && !['disabled', 'base'].includes(here.type);
  if (active) {
    html += field('Channel', `<input type="number" id="eCh" min="1" max="16" value="${m.channel}">`);
    if (['cc', 'note', 'program'].includes(m.type)) {
      html += field(m.type === 'cc' ? 'CC number' : m.type === 'note' ? 'Note' : 'Program', `<input type="number" id="eNum" min="0" max="127" value="${m.number}">`);
    }
    if (m.type !== 'program' || c.kind !== 'button') {
      html += field(c.kind === 'button' ? 'Off / on' : 'Range', `<input type="number" id="eMin" min="0" max="127" value="${m.min}"> – <input type="number" id="eMax" min="0" max="127" value="${m.max}">`);
    }
    if (c.kind === 'button') {
      const bm = m.flags & CFG.BUTTON_MASK;
      html += field('Button', `<select id="eBtn"><option value="0" ${bm === 0 ? 'selected' : ''}>momentary</option><option value="1" ${bm === 1 ? 'selected' : ''}>toggle (LED shows the state)</option><option value="2" ${bm === 2 ? 'selected' : ''}>trigger (press only)</option></select>`);
    }
    if (c.kind === 'fader') {
      const t = m.flags & CFG.TAKEOVER_MASK;
      html += field('Takeover', `<select id="eTk"><option value="0" ${t === 0 ? 'selected' : ''}>global (${state.cfg.fader_takeover})</option><option value="4" ${t === 4 ? 'selected' : ''}>jump</option><option value="8" ${t === 8 ? 'selected' : ''}>pickup</option></select>`);
    }
    if (c.kind === 'knob') {
      const a = m.flags & CFG.ACCEL_MASK;
      html += field('Acceleration', `<select id="eAc"><option value="0" ${a === 0 ? 'selected' : ''}>global (${state.cfg.accel ? 'on' : 'off'})</option><option value="16" ${a === 16 ? 'selected' : ''}>off</option><option value="32" ${a === 32 ? 'selected' : ''}>on</option></select>`);
    }
  }
  html += `<div class="row"><label></label><button id="eAll">Copy to all pages</button>${here ? '<button id="eClear">Remove mapping</button>' : ''}</div>`;
  html += `<p class="hint">"Copy to all pages" moves this mapping to the all-pages layer (${state.shift ? 'Shift' : 'normal'}), so it applies on every page unless a page maps the control itself.</p>`;
  p.innerHTML = html;

  const num = (sel, def) => { const e = $(sel); return e ? Math.max(0, Math.min(127, +e.value || 0)) : def; };
  const update = () => {
    const type = $('#eType').value;
    if (type === 'inherit') { setHere(id, null); return; }
    const cur = findHere(id) || { channel: defaultChannel(id), number: 0, flags: 0, min: 0, max: 127 };
    let flags = cur.flags;
    if ($('#eBtn')) flags = (flags & ~CFG.BUTTON_MASK) | +$('#eBtn').value;
    if ($('#eTk')) flags = (flags & ~CFG.TAKEOVER_MASK) | +$('#eTk').value;
    if ($('#eAc')) flags = (flags & ~CFG.ACCEL_MASK) | +$('#eAc').value;
    setHere(id, {
      type, channel: $('#eCh') ? Math.max(1, Math.min(16, +$('#eCh').value || 1)) : cur.channel,
      number: num('#eNum', cur.number), flags, min: num('#eMin', cur.min), max: num('#eMax', cur.max),
    });
  };
  p.querySelectorAll('select, input').forEach(e => { e.onchange = update; });
  $('#eAll').onclick = () => {
    const m2 = findHere(id) || (r && r.mapping);
    if (!m2) { toast('nothing mapped here to copy'); return; }
    const copy = Object.assign({}, m2);
    state.cfg.mappings = state.cfg.mappings.filter(x => !(x.control === id && x.shift === state.shift));
    state.cfg.mappings.push(Object.assign(copy, { control: id, page: 0, shift: state.shift }));
    state.page = 0;
    refreshAll();
  };
  if ($('#eClear')) $('#eClear').onclick = () => setHere(id, null);
}

function defaultChannel(id) {
  const c = CONTROLS[id];
  const tb = state.cfg.track_base[(state.page || 1) - 1] || 0;
  return c.strip !== undefined ? ((tb + c.strip) % 16) + 1 : 1;
}

// ------------------------------------------------------------------ global settings

function renderGlobals() {
  const p = $('#globalPanel');
  if (!state.cfg) { p.innerHTML = '<h2>Settings</h2><p class="hint">Connect or simulate a mixer.</p>'; return; }
  const g = state.cfg;
  const opt = (v, cur, label) => `<option value="${v}" ${String(cur) === String(v) ? 'selected' : ''}>${label}</option>`;
  let html = '<h2>Settings</h2>';
  html += field('Base profile', `<select id="gBase">${opt('opxy', g.base, 'OP-XY')}${opt('daw', g.base, 'DAW (Mackie)')}</select>`);
  html += field('Pages', `<input type="number" id="gPages" min="1" max="8" value="${g.pages}">`);
  html += '<h3>Track of strip 1, per page</h3><div class="tb">' +
    g.track_base.map((t, i) => `<label>page ${i + 1}<input type="number" class="gTb" data-i="${i}" min="1" max="16" value="${t + 1}"></label>`).join('') + '</div>';
  html += '<h3>Faders</h3>';
  html += field('Takeover', `<select id="gTk">${opt('pickup', g.fader_takeover, 'pickup')}${opt('jump', g.fader_takeover, 'jump')}</select>`);
  html += field('Mismatch LED', `<input type="checkbox" id="gLed" ${g.fader_led ? 'checked' : ''}>`);
  html += '<h3>Values</h3>';
  html += field('Follow incoming', `<input type="checkbox" id="gFollow" ${g.follow_incoming ? 'checked' : ''}><span class="hint">CC / pitch bend / pressure received set the known value</span>`);
  html += field('Remember', `<input type="checkbox" id="gRemember" ${g.remember_values ? 'checked' : ''}><span class="hint">across power cycles</span>`);
  html += '<h3>Knob acceleration</h3>';
  html += field('Acceleration', `<input type="checkbox" id="gAccel" ${g.accel ? 'checked' : ''}>`);
  html += field('x3 under', `<input type="number" id="gA3" min="1" max="100" value="${g.accel_ms_x3}"> ms between steps`);
  html += field('x2 under', `<input type="number" id="gA2" min="1" max="100" value="${g.accel_ms_x2}"> ms`);
  p.innerHTML = html;
  const upd = () => {
    g.base = $('#gBase').value;
    g.pages = Math.max(1, Math.min(8, +$('#gPages').value || 1));
    document.querySelectorAll('.gTb').forEach(e => { g.track_base[+e.dataset.i] = Math.max(1, Math.min(16, +e.value || 1)) - 1; });
    g.fader_takeover = $('#gTk').value;
    g.fader_led = $('#gLed').checked;
    g.follow_incoming = $('#gFollow').checked;
    g.remember_values = $('#gRemember').checked;
    g.accel = $('#gAccel').checked;
    g.accel_ms_x3 = Math.max(1, Math.min(100, +$('#gA3').value || 6));
    g.accel_ms_x2 = Math.max(1, Math.min(100, +$('#gA2').value || 12));
    if (state.page > g.pages) state.page = 0;
    refreshAll();
  };
  p.querySelectorAll('select, input').forEach(e => { e.onchange = upd; });
}

function refreshAll() {
  refreshSurface();
  renderEditor();
  renderGlobals();
  markDirty();
}

// ------------------------------------------------------------------ monitor

function monitor(line) {
  const m = $('#monitor');
  const atBottom = m.scrollTop + m.clientHeight >= m.scrollHeight - 4;
  m.textContent += line + '\n';
  if (m.textContent.length > 20000) m.textContent = m.textContent.slice(-15000);
  if (atBottom) m.scrollTop = m.scrollHeight;
}

$('#btnClearMon').onclick = () => { $('#monitor').textContent = ''; };

function describeMidi(b) {
  const t = b[0] & 0xF0, ch = (b[0] & 15) + 1;
  switch (t) {
    case 0x80: return `note off  ch${ch} ${b[1]}`;
    case 0x90: return `note on   ch${ch} ${b[1]} vel ${b[2]}`;
    case 0xB0: return `CC        ch${ch} ${b[1]} = ${b[2]}`;
    case 0xC0: return `program   ch${ch} ${b[1]}`;
    case 0xD0: return `pressure  ch${ch} ${b[1]}`;
    case 0xE0: return `pitchbend ch${ch} ${(b[2] << 7) | b[1]}`;
    default: return b.map(x => x.toString(16).padStart(2, '0')).join(' ');
  }
}

function onEvent(control, value, page, shift) {
  const el = document.querySelector(`.ctl[data-id="${control}"]`);
  if (el) {
    el.classList.add('hit');
    clearTimeout(el.hitTimer);
    el.hitTimer = setTimeout(() => el.classList.remove('hit'), 250);
  }
  if (control >= 8 && control < 16) {
    const cap = $(`#cap${control - 8}`);
    if (cap) cap.style.bottom = `calc(${(value / 1023) * 100}% - ${(value / 1023) * 14}px)`;
  }
  const c = CONTROLS[control];
  const v = c.kind === 'knob' ? (value > 32767 ? value - 65536 : value) : value;
  monitor(`▸ ${c ? c.name : control}  ${c.kind === 'button' ? (v ? 'down' : 'up') : v}  (page ${page}${shift ? ', Shift' : ''})`);
}

// ------------------------------------------------------------------ connection

async function useLink(link) {
  if (state.link && state.link.close) state.link.close();
  state.link = link;
  link.onEvent = onEvent;
  link.onMidi = b => monitor('  ' + describeMidi(b));
  try {
    const h = await link.hello();
    state.cfg = await link.readConfig();
    state.saved = JSON.stringify(state.cfg);
    $('#status').innerHTML = `<b>${link.name}</b> · firmware ${h.fw} · page ${h.page}`;
    ['#btnReload', '#btnExport', '#btnImport', '#btnFactory'].forEach(s => { $(s).disabled = false; });
    if ($('#chkEvents').checked) await link.events(true);
    refreshAll();
    toast('configuration loaded');
  } catch (e) {
    toast(e.message, true);
  }
}

$('#btnConnect').onclick = async () => {
  try { await useLink(await WebMidiLink.open()); } catch (e) { toast(e.message, true); }
};
$('#btnSim').onclick = () => useLink(new SimulatedLink());
$('#btnReload').onclick = async () => {
  try { state.cfg = await state.link.readConfig(); state.saved = JSON.stringify(state.cfg); refreshAll(); toast('reloaded from the mixer'); } catch (e) { toast(e.message, true); }
};
$('#btnSave').onclick = async () => {
  try {
    $('#btnSave').disabled = true;
    await state.link.writeConfig(state.cfg);
    state.saved = JSON.stringify(state.cfg);
    markDirty();
    toast('saved to the mixer');
  } catch (e) { toast(e.message, true); markDirty(); }
};
$('#btnFactory').onclick = async () => {
  if (!confirm('Erase every mapping and setting on the mixer and go back to the defaults?')) return;
  try { await state.link.factoryReset(); state.cfg = await state.link.readConfig(); state.saved = JSON.stringify(state.cfg); refreshAll(); toast('factory settings'); } catch (e) { toast(e.message, true); }
};
$('#btnExport').onclick = () => {
  const blob = new Blob([JSON.stringify(state.cfg, null, 1)], { type: 'application/json' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'smc-mixer-config.json';
  a.click();
  URL.revokeObjectURL(a.href);
};
$('#btnImport').onclick = () => $('#fileImport').click();
$('#fileImport').onchange = async e => {
  const f = e.target.files[0];
  if (!f) return;
  try {
    const cfg = Object.assign(defaultConfig(), JSON.parse(await f.text()));
    encodeConfig(cfg);                  // validates
    state.cfg = cfg;
    refreshAll();
    toast('imported: press "Save to mixer" to send it');
  } catch (err) { toast(`not a configuration: ${err.message}`, true); }
  e.target.value = '';
};
$('#chkEvents').onchange = async e => {
  if (state.link) { try { await state.link.events(e.target.checked); } catch (err) { toast(err.message, true); } }
};
document.querySelectorAll('#layers button').forEach(b => { b.onclick = () => { state.shift = b.dataset.shift === '1'; refreshAll(); }; });
window.addEventListener('beforeunload', e => { if (state.cfg && JSON.stringify(state.cfg) !== state.saved) { e.preventDefault(); e.returnValue = ''; } });

buildSurface();
renderGlobals();

// ?sim opens the simulated mixer right away (demos, tests)
if (new URLSearchParams(location.search).has('sim')) useLink(new SimulatedLink());
