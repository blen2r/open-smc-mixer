// SMC-Mixer editor: the mixer drawing, the mapping editor, global settings, monitor.
'use strict';

const $ = s => document.querySelector(s);
const state = { profileOpen: (() => { try { return localStorage.getItem('smc-mixer-profile-open') === '1'; } catch (e) { return false; } })(), link: null, cfg: null, saved: '', page: 0, shift: false, mode: 'daw', sel: null, loadedMode: null,
                templates: [], faders: new Array(8).fill(0) };

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
  const r = resolveMapping(cfg, id, state.page, state.shift, state.mode);
  const res = reservedFor(id, state.shift);
  if (res) return { cls: 'reserved', text: res, full: `${res} (reserved)` };
  if (!r || r.base) return { cls: 'base', text: baseShort(cfg, id, state.page, state.shift, state.mode), full: baseDescription(cfg, id, state.page, state.shift, state.mode) };
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
    strip.appendChild(ctlEl(s, 'encoder'));
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
    el.classList.remove('here', 'inherited', 'base', 'disabled', 'reserved');
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
  document.querySelectorAll('#modes button').forEach(b => b.classList.toggle('on', b.dataset.mode === state.mode));
}

// ------------------------------------------------------------------ control editor

function select(id) {
  state.sel = id;
  refreshAll();
}

function findHere(id) {
  return state.cfg.mappings.find(m => m.mode === state.mode && m.control === id && m.page === state.page && m.shift === state.shift);
}

function setHere(id, m) {
  const ms = state.cfg.mappings.filter(x => !(x.mode === state.mode && x.control === id && x.page === state.page && x.shift === state.shift));
  if (m) ms.push(Object.assign({ mode: state.mode, control: id, page: state.page, shift: state.shift }, m));
  state.cfg.mappings = ms;
  refreshAll();
}

function field(label, html, tip) {
  return `<div class="row"><label>${label}${tip ? tipIcon(tip) : ''}</label>${html}</div>`;
}

// a "?" that explains the item next to it, on hover or keyboard focus
function tipIcon(text) {
  return `<span class="tip" tabindex="0" data-tip="${esc(text)}" aria-label="${esc(text)}">?</span>`;
}

function renderEditor() {
  const p = $('#editPanel');
  if (!state.cfg || state.sel === null) {
    p.innerHTML = '<h2>Control</h2><p class="hint">Select a control on the mixer drawing.</p>';
    return;
  }
  const id = state.sel, c = CONTROLS[id];
  const here = findHere(id);
  const r = resolveMapping(state.cfg, id, state.page, state.shift, state.mode);
  const layer = `${state.mode === 'daw' ? 'DAW' : 'CC'} mode, ${state.page ? 'page ' + state.page : 'all pages'}${state.shift ? ' + Shift' : ''}`;
  const res = reservedFor(id, state.shift);
  if (res) {
    p.innerHTML = `<h2>${c.name}</h2><div class="src">${state.shift ? 'Shift + ' : ''}${c.name} is reserved: <b>${res}</b>.</div>`;
    return;
  }
  let src;
  if (here) src = `Mapped on <b>${layer}</b>.`;
  else if (r && !r.base) src = `Not mapped on ${layer}: inherits <b>${mappingText(r.mapping)}</b> from ${r.from}.`;
  else src = `Not mapped on ${layer}: ${state.mode === 'daw' ? 'DAW mode' : 'CC mode'} does <b>${baseDescription(state.cfg, id, state.page, state.shift, state.mode)}</b>.`;

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
    if (c.kind === 'encoder') {
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
    state.cfg.mappings = state.cfg.mappings.filter(x => !(x.mode === state.mode && x.control === id && x.shift === state.shift));
    state.cfg.mappings.push(Object.assign(copy, { mode: state.mode, control: id, page: 0, shift: state.shift }));
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

// The CC mode profile (what a template sets: the card above the drawing) and the mixer's mode and
// personal preferences (the Settings panel). Both write into state.cfg through one handler.
function renderGlobals() {
  const p = $('#globalPanel'), pp = $('#profilePanel');
  if (!state.cfg) {
    p.innerHTML = '<h2>Settings</h2><p class="hint">Connect or simulate a mixer.</p>';
    pp.innerHTML = '<div class="phead"><h2>CC mode profile</h2></div>';
    return;
  }
  const g = state.cfg;
  const opt = (v, cur, label) => `<option value="${v}" ${String(cur) === String(v) ? 'selected' : ''}>${label}</option>`;
  const ccIn = (id, v, none) => `<input type="number" id="${id}" min="0" max="127" value="${v === null ? '' : v}" placeholder="${none ? 'none' : ''}">`;

  // the profile card
  let ph = '<div class="phead"><h2>CC mode profile</h2>' +
    `<span class="src">Template: <b>${g.name ? esc(g.name) : 'custom (edited by hand)'}</b></span>` +
    '<button id="gPick">Choose a template…</button><button id="gSaveTpl">Save as template…</button>' +
    `<span class="spacer"></span><button id="gToggle">${state.profileOpen ? 'Hide details' : 'Show details'}</button></div>`;
  ph += '<p class="hint">What CC mode sends where nothing is mapped, for one device. A template sets this card and CC mode\'s mappings; your preferences (Settings) and DAW mode stay. Backup / Restore (top) save the whole mixer instead.</p>';
  if (state.profileOpen) {
    let c1 = '<h3>Controls</h3>';
    c1 += field('Fader CC', ccIn('gCcF', g.cc_fader), 'Each fader sends this CC on its strip\'s track channel (OP-XY: 7, volume).');
    c1 += field('Shift + fader', ccIn('gCcFS', g.cc_fader_shift, true), 'While Shift is held, faders send this CC instead (OP-XY: 10, pan). Empty: the same as without Shift.');
    c1 += field('Encoder CC', ccIn('gCcK', g.cc_encoder, true), 'Each encoder sends this CC on its strip\'s track channel, as an absolute value the mixer keeps. Empty: encoders send nothing, unless a focus group is on.');
    c1 += field('Mute CC', ccIn('gCcM', g.cc_mute, true), 'Mute toggles this CC between 127 (muted) and 0; the LED shows it. Empty: Mute sends nothing.');
    c1 += field('Solo', `<input type="checkbox" id="gSolo" ${g.solo_mutes ? 'checked' : ''}>`, 'For devices without a solo CC: Solo mutes every other track (with the Mute CC) while any track is soloed, and puts their own mute states back when solo ends.');
    c1 += field('Special modes', `<input type="checkbox" id="gSpecial" ${g.special_modes ? 'checked' : ''}>`, 'Shift + Mute / Solo / Rec / Track stop turn that strip\'s fader into mod wheel / pitch bend / channel pressure / pan (CC 10). The button blinks while it is on; press it again to go back to the fader CC.');
    const focusRow = (key, cls) => '<div class="fg">' + g[key].map((v, k) =>
      `<label>${k + 1}<input type="number" class="${cls}" data-k="${k}" min="0" max="127" value="${v === null ? '' : v}" placeholder="–"></label>`).join('') + '</div>';
    let c2 = `<h3>Focus groups${tipIcon('Rec (or Track stop) on a strip turns its group on for that strip\'s track: each encoder with a CC in the row sends that CC on the track\'s channel; press again: off. Typical use: the track\'s parameters on the encoders.')}</h3><p class="hint">One box per encoder (1-8).</p>`;
    c2 += `<div class="flabel">Rec${tipIcon('Encoders with a CC here send it on the track whose Rec you pressed. An empty box leaves that encoder as it is. All empty: Rec does nothing.')}</div>${focusRow('focus_rec', 'gFoR')}`;
    c2 += `<div class="flabel">Track stop${tipIcon('The same for the Track stop button (the bottom button of each strip). If both rows use the same encoder, only one group can be on: turning one on blinks both buttons and turns the other off.')}</div>${focusRow('focus_stop', 'gFoS')}`;
    if (focusOverlap(g)) c2 += '<p class="hint">These groups overlap (same encoder in both): only one can be on at a time.</p>';
    let c3 = '<h3>Pages and tracks</h3>';
    c3 += field('Pages', `<input type="number" id="gPages" min="1" max="8" value="${g.pages}">`, 'How many pages Shift + Rewind / Fast-forward go through (1-8).');
    c3 += `<div class="flabel">Track of strip 1${tipIcon('Per page: strip 1 is this track, strips 2-8 the next ones (after 16: 1 again). The track is the MIDI channel of the strip\'s fader, encoder and buttons. To send one control somewhere else, map it.')}</div><div class="tb">` +
      g.track_base.map((t, i) => `<label>page ${i + 1}<input type="number" class="gTb" data-i="${i}" min="1" max="16" value="${t + 1}"></label>`).join('') + '</div>';
    c3 += field('Follow incoming', `<input type="checkbox" id="gFollow" ${g.follow_incoming ? 'checked' : ''}>`, 'CC / pitch bend / pressure the device sends back become the controls\' known values: encoders continue from them, faders pick them up. Never sent back out (no loops).');
    ph += `<div class="pgrid"><div>${c1}</div><div>${c2}</div><div>${c3}</div></div>`;
  }
  pp.innerHTML = ph;

  // the settings panel
  let html = '<h2>Settings</h2>';
  html += field('Mixer mode', `<select id="gMode">${opt('daw', g.mode, 'DAW (Mackie, as stock)')}${opt('cc', g.mode, 'CC')}</select>`, 'DAW: the stock Mackie Control layout, for a computer DAW. CC: the profile above, for a device. On the mixer: Shift + Left = DAW, Shift + Right = CC (the lit arrow shows the mode while Shift is held). Kept across power cycles.');
  html += '<h3>Faders</h3>';
  html += field('Takeover', `<select id="gTk">${opt('pickup', g.fader_takeover, 'pickup')}${opt('jump', g.fader_takeover, 'jump')}</select>`, 'Pickup: a fader that is not where its value is sends nothing until you move it through the value (no jumps). Jump: it sends its position at once. A mapping can override this.');
  html += field('Mismatch LED', `<input type="checkbox" id="gLed" ${g.fader_led ? 'checked' : ''}>`, 'The LED above a fader is lit while the fader is not at its value (it needs picking up) and goes off when it gets there.');
  html += '<h3>Values</h3>';
  html += field('Remember', `<input type="checkbox" id="gRemember" ${g.remember_values ? 'checked' : ''}>`, 'Keep the known values (where each fader and encoder value is) across power cycles; saved 5 s after the last change.');
  html += '<h3>Encoder acceleration</h3>';
  html += field('Acceleration', `<input type="checkbox" id="gAccel" ${g.accel ? 'checked' : ''}>`, 'Turning an encoder fast moves the value further per step. A mapping can turn it on or off for one encoder.');
  html += field('x3 under', `<input type="number" id="gA3" min="1" max="100" value="${g.accel_ms_x3}"> ms`, 'Steps closer together than this count three times.');
  html += field('x2 under', `<input type="number" id="gA2" min="1" max="100" value="${g.accel_ms_x2}"> ms`, 'Steps closer together than this (and not under the x3 time) count twice.');
  p.innerHTML = html;

  const cc = (sel, none) => { const v = $(sel).value.trim(); return v === '' && none ? null : Math.max(0, Math.min(127, +v || 0)); };
  const box = el => { const v = el.value.trim(); return v === '' ? null : Math.max(0, Math.min(127, +v || 0)); };
  const upd = e => {
    const before = JSON.stringify(TEMPLATE_SETTINGS.map(k => g[k]));
    g.mode = $('#gMode').value;
    if (state.profileOpen) {
      g.cc_fader = cc('#gCcF');
      g.cc_fader_shift = cc('#gCcFS', true);
      g.cc_mute = cc('#gCcM', true);
      g.solo_mutes = $('#gSolo').checked;
      g.cc_encoder = cc('#gCcK', true);
      document.querySelectorAll('.gFoR').forEach(el => { g.focus_rec[+el.dataset.k] = box(el); });
      document.querySelectorAll('.gFoS').forEach(el => { g.focus_stop[+el.dataset.k] = box(el); });
      g.special_modes = $('#gSpecial').checked;
      g.pages = Math.max(1, Math.min(8, +$('#gPages').value || 1));
      document.querySelectorAll('.gTb').forEach(el => { g.track_base[+el.dataset.i] = Math.max(1, Math.min(16, +el.value || 1)) - 1; });
      g.follow_incoming = $('#gFollow').checked;
    }
    g.fader_takeover = $('#gTk').value;
    g.fader_led = $('#gLed').checked;
    g.remember_values = $('#gRemember').checked;
    g.accel = $('#gAccel').checked;
    g.accel_ms_x3 = Math.max(1, Math.min(100, +$('#gA3').value || 6));
    g.accel_ms_x2 = Math.max(1, Math.min(100, +$('#gA2').value || 12));
    // the profile edited by hand is no longer the template's
    if (JSON.stringify(TEMPLATE_SETTINGS.map(k => g[k])) !== before) g.name = '';
    if (e && e.target.id === 'gMode') state.mode = g.mode;
    if (state.page > g.pages) state.page = 0;
    refreshAll();
  };
  [p, pp].forEach(panel => panel.querySelectorAll('select, input').forEach(el => { el.onchange = upd; }));
  $('#gPick').onclick = openPicker;
  $('#gSaveTpl').onclick = saveTemplate;
  $('#gToggle').onclick = () => {
    state.profileOpen = !state.profileOpen;
    try { localStorage.setItem('smc-mixer-profile-open', state.profileOpen ? '1' : '0'); } catch (err) { /* private mode */ }
    renderGlobals();
  };
}

function esc(t) {
  return String(t).replace(/[&<>"]/g, ch => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[ch]));
}

// ------------------------------------------------------------------ templates

const STARS_KEY = 'smc-mixer-stars';
function stars() {
  try { return new Set(JSON.parse(localStorage.getItem(STARS_KEY) || '[]')); } catch (e) { return new Set(); }
}
function setStar(name, on) {
  const s = stars();
  if (on) s.add(name); else s.delete(name);
  try { localStorage.setItem(STARS_KEY, JSON.stringify([...s])); } catch (e) { /* private mode */ }
}

// templates/index.json (in the repository) and templates/local/index.json (yours, not committed);
// both written by host/templates_index.py
async function loadTemplates() {
  const list = [];
  for (const [dir, local] of [['templates/', false], ['templates/local/', true]]) {
    try {
      const r = await fetch(dir + 'index.json', { cache: 'no-store' });
      if (!r.ok) continue;
      for (const t of await r.json()) list.push(Object.assign({}, t, { base: dir, local }));
    } catch (e) { /* none */ }
  }
  state.templates = list;
  return list;
}

async function openPicker() {
  await loadTemplates();
  const dlg = $('#picker');
  dlg.showModal();
  renderPicker();
}

function renderPicker() {
  const q = $('#pkSearch').value.trim().toLowerCase();
  const onlyStars = $('#pkStars').checked;
  const st = stars();
  const rows = state.templates
    .filter(t => !onlyStars || st.has(t.name))
    .filter(t => !q || [t.name, t.device, t.author, t.description].join(' ').toLowerCase().includes(q))
    .sort((a, b) => (st.has(b.name) - st.has(a.name)) || a.name.localeCompare(b.name));
  const box = $('#pkList');
  if (!state.templates.length) {
    box.innerHTML = '<p class="hint">No templates found (templates/index.json). Run host/templates_index.py.</p>';
    return;
  }
  box.innerHTML = rows.map((t, i) => `<div class="tpl">
      <button class="star ${st.has(t.name) ? 'on' : ''}" data-i="${i}" title="favourite">${st.has(t.name) ? '★' : '☆'}</button>
      <div class="tplText"><b>${esc(t.name)}</b>${t.local ? ' <span class="hint">(local)</span>' : ''}<br>
        <span class="hint">${esc(t.device)}${t.description ? ' · ' + esc(t.description) : ''}</span>
        ${t.doc ? `<br><a href="${esc(t.base + t.doc)}" target="_blank" rel="noopener">How it's mapped</a>` : ''}</div>
      <button class="apply" data-i="${i}">Use</button></div>`).join('') || '<p class="hint">No template matches.</p>';
  box.querySelectorAll('.star').forEach(b => { b.onclick = () => { const t = rows[+b.dataset.i]; setStar(t.name, !st.has(t.name)); renderPicker(); }; });
  box.querySelectorAll('.apply').forEach(b => { b.onclick = () => useTemplate(rows[+b.dataset.i]); });
}

async function useTemplate(entry) {
  try {
    const r = await fetch(entry.base + entry.file, { cache: 'no-store' });
    const t = await r.json();
    const problems = checkTemplate(t);
    if (problems.length) throw new Error(problems.join('; '));
    const ccMaps = state.cfg.mappings.filter(m => m.mode === 'cc').length;
    if (ccMaps && !confirm(`Replace CC mode's profile and its ${ccMaps} mapping(s) with "${t.name}"? (DAW mode is not touched.)`)) return;
    state.cfg = applyTemplate(state.cfg, t);
    state.mode = 'cc';
    $('#picker').close();
    refreshAll();
    toast(`"${t.name}" loaded: press "Save to mixer" to send it (Shift + Right on the mixer: CC mode)`);
  } catch (e) { toast(`template: ${e.message}`, true); }
}

function download(name, text, type) {
  const blob = new Blob([text], { type });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

// A description of a template's mapping, to start its .md from
function focusDoc(g) {
  const used = g.map((v, k) => (v === null ? null : `encoder ${k + 1} → CC ${v}`)).filter(Boolean);
  return used.length ? `focus on that track (again: off): ${used.join(', ')}` : 'nothing';
}
function templateDoc(t) {
  const s = t.settings;
  const cc = v => (v === null ? '—' : `CC ${v}`);
  const lines = [`# ${t.name}`, '', `For: ${t.device}. By ${t.author}.`, '', t.description || '_What it is for._', '',
    '## CC mode', '', '| control | sends |', '|---|---|',
    `| fader | ${cc(s.cc_fader)} on the strip's channel |`,
    `| Shift + fader | ${s.cc_fader_shift === null ? 'as without Shift' : cc(s.cc_fader_shift)} |`,
    `| encoder | ${cc(s.cc_encoder)} on the strip's channel |`,
    `| Mute | ${s.cc_mute === null ? 'nothing' : `toggles ${cc(s.cc_mute)} 127 / 0`} |`,
    `| Solo | ${s.solo_mutes && s.cc_mute !== null ? 'mutes the other tracks' : 'nothing'} |`,
    `| Rec | ${focusDoc(s.focus_rec)} |`,
    `| Track stop | ${focusDoc(s.focus_stop)} |`,
    `| Shift + Mute / Solo / Rec / Track stop | ${s.special_modes ? 'fader special modes: mod wheel / pitch bend / pressure / pan' : 'as without Shift'} |`,
    '', `Pages: ${s.pages}; strip 1 is track ${s.track_base.slice(0, s.pages).map(x => x + 1).join(' / ')} on page ${[...Array(s.pages).keys()].map(x => x + 1).join(' / ')}.`, ''];
  if (t.mappings.length) {
    lines.push('## Mappings', '', '| control | layer | sends |', '|---|---|---|');
    for (const m of t.mappings) {
      lines.push(`| ${CONTROLS[m.control].name} | ${m.page ? 'page ' + m.page : 'all pages'}${m.shift ? ' + Shift' : ''} | ${mappingText(m)}${m.min || m.max !== 127 ? ` (${m.min}..${m.max})` : ''} |`);
    }
    lines.push('');
  }
  lines.push('## Notes', '', '_Anything the device needs (its MIDI settings, channels), and what does not work._', '');
  return lines.join('\n');
}

function saveTemplate() {
  const author = prompt('Your name or GitHub handle (the template is named after it, e.g. "OP-XY - blen2r"):');
  if (!author) return;
  const device = prompt('The device it is for (e.g. "Teenage Engineering OP-XY"):');
  if (!device) return;
  const name = prompt(`Template name (at most ${CFG.NAME_LEN} characters, must contain " - ${author}"):`, `${device.split(' ').pop()} - ${author}`);
  if (!name) return;
  const description = prompt('One line on what it does (optional):') || '';
  const t = templateFromConfig(state.cfg, { name, device, author, description });
  const problems = checkTemplate(t);
  if (problems.length) { toast(problems.join('; '), true); return; }
  const file = name.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');
  download(`${file}.json`, JSON.stringify(t, null, 1) + '\n', 'application/json');
  download(`${file}.md`, templateDoc(t), 'text/markdown');
  state.cfg.name = name;
  refreshAll();
  toast(`saved ${file}.json and ${file}.md: put them in editor/templates/local/ (yours) or editor/templates/ (to share), then run host/templates_index.py`);
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
  const v = c.kind === 'encoder' ? (value > 32767 ? value - 65536 : value) : value;
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
    state.mode = state.loadedMode = state.cfg.mode;
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
  try { state.cfg = await state.link.readConfig(); state.saved = JSON.stringify(state.cfg); state.mode = state.loadedMode = state.cfg.mode; refreshAll(); toast('reloaded from the mixer'); } catch (e) { toast(e.message, true); }
};
$('#btnSave').onclick = async () => {
  try {
    $('#btnSave').disabled = true;
    // the mode switched on the mixer (Shift + Left / Right) since it was read wins, unless it was
    // changed here too
    const h = await state.link.hello();
    if (state.cfg.mode === state.loadedMode && h.mode !== undefined && CFG.MODES[h.mode] !== state.cfg.mode) state.cfg.mode = CFG.MODES[h.mode];
    await state.link.writeConfig(state.cfg);
    state.loadedMode = state.cfg.mode;
    state.saved = JSON.stringify(state.cfg);
    markDirty();
    toast('saved to the mixer');
  } catch (e) { toast(e.message, true); markDirty(); }
};
$('#btnFactory').onclick = async () => {
  if (!confirm('Erase every mapping and setting on the mixer and go back to the defaults?')) return;
  try { await state.link.factoryReset(); state.cfg = await state.link.readConfig(); state.saved = JSON.stringify(state.cfg); state.mode = state.loadedMode = state.cfg.mode; refreshAll(); toast('factory settings'); } catch (e) { toast(e.message, true); }
};
$('#btnExport').onclick = () => {
  const blob = new Blob([JSON.stringify(state.cfg, null, 1)], { type: 'application/json' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = `smc-mixer-backup-${new Date().toISOString().slice(0, 10)}.json`;
  a.click();
  URL.revokeObjectURL(a.href);
};
$('#btnImport').onclick = () => $('#fileImport').click();
$('#fileImport').onchange = async e => {
  const f = e.target.files[0];
  if (!f) return;
  try {
    const obj = JSON.parse(await f.text());
    if (obj.settings && obj.author) {   // a template file: apply it as one (DAW mode, preferences stay)
      const problems = checkTemplate(obj);
      if (problems.length) throw new Error(problems.join('; '));
      state.cfg = applyTemplate(state.cfg, obj);
      refreshAll();
      toast(`template "${obj.name}" applied: press "Save to mixer" to send it`);
    } else {
      const cfg = normalizeConfig(obj);
      encodeConfig(cfg);                // validates
      state.cfg = cfg;
      refreshAll();
      toast('backup restored here: press "Save to mixer" to send it');
    }
  } catch (err) { toast(`not a backup or a template: ${err.message}`, true); }
  e.target.value = '';
};
$('#chkEvents').onchange = async e => {
  if (state.link) { try { await state.link.events(e.target.checked); } catch (err) { toast(err.message, true); } }
};
document.querySelectorAll('#layers button').forEach(b => { b.onclick = () => { state.shift = b.dataset.shift === '1'; refreshAll(); }; });
document.querySelectorAll('#modes button').forEach(b => { b.onclick = () => { state.mode = b.dataset.mode; refreshAll(); }; });
$('#pkSearch').oninput = renderPicker;
$('#pkStars').onchange = renderPicker;
$('#pkClose').onclick = () => $('#picker').close();
window.addEventListener('beforeunload', e => { if (state.cfg && JSON.stringify(state.cfg) !== state.saved) { e.preventDefault(); e.returnValue = ''; } });

buildSurface();
renderGlobals();

// ?sim opens the simulated mixer right away (demos, tests)
if (new URLSearchParams(location.search).has('sim')) useLink(new SimulatedLink());
