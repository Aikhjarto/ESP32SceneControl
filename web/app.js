'use strict';
// ServoScene web UI. Talks to the ESP exclusively through the WebSocket command
// protocol (the same FluidNC-style commands as the serial port).

const PROFILES = ['const', 'trap', 'tri', 'sine', 'bell'];
const MODES = ['once', 'return', 'pingpong'];
const DRIVERS = ['auto', 'ledc', 'mcpwm'];
const ENUMS = { profile: PROFILES, mode: MODES, driver: DRIVERS };
const MAX_SERVOS = 64;
const REPLY_TIMEOUT_MS = 10000;
const SIGNED_FIELDS = new Set(['trim_us']);  // may be negative: iOS decimal keypads have no minus key

const $ = (sel, root = document) => root.querySelector(sel);

function el(tag, attrs = {}, ...children) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (v === undefined || v === null || v === false) continue;
    if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
    else if (k === 'class') e.className = v;
    else e.setAttribute(k, v === true ? '' : v);
  }
  for (const c of children.flat()) if (c !== null && c !== undefined) e.append(c);
  return e;
}

// ---------------------------------------------------------------------------
// State

const model = new Map();   // "/servo/3/start_deg" or "AP/SSID" → value string
const hwInfo = {};         // servo → "gpio.13 ledc-hs.0"
const positions = {};      // servo → current output angle
let status = { run: false, t: 0, cyc: 0, stop: new Set(), rejoin: new Set(), test: new Set() };
let statusAt = performance.now();
let dirty = false;
let bootWarnings = [];

const num = (path, dflt = 0) => {
  const v = parseFloat(model.get(path));
  return Number.isFinite(v) ? v : dflt;
};
const sp = (i, f) => `/servo/${i}/${f}`;

function servoIndices() {
  const out = [];
  for (let i = 0; i < MAX_SERVOS; i++) {
    const pin = model.get(sp(i, 'pin'));
    if (pin && pin !== 'none') out.push(i);
  }
  return out;
}
const servoActive = i => model.get(sp(i, 'enable')) === 'true' && (model.get(sp(i, 'pin')) || 'none') !== 'none';
const servoName = i => model.get(sp(i, 'name')) || `Servo ${i}`;

// ---------------------------------------------------------------------------
// Motion math (mirrors src/core/Motion.cpp) for the plots and timeline

function profilePos(p, u, accelPct) {
  if (u <= 0) return 0;
  if (u >= 1) return 1;
  switch (p) {
    case 'const': return u;
    case 'trap':
    case 'tri': {
      const a = p === 'tri' ? 0.5 : Math.min(Math.max(accelPct, 0), 50) / 100;
      if (a <= 0) return u;
      const vp = 1 / (1 - a);
      if (u < a) return vp * u * u / (2 * a);
      if (u <= 1 - a) return vp * (u - a / 2);
      const r = 1 - u;
      return 1 - vp * r * r / (2 * a);
    }
    case 'sine': return (1 - Math.cos(Math.PI * u)) / 2;
    default: return u * u * u * (10 + u * (-15 + 6 * u));
  }
}

function servoParams(i) {
  return {
    start: num(sp(i, 'start_deg')), stop: num(sp(i, 'stop_deg')),
    delay: num(sp(i, 'delay_ms')), dur: num(sp(i, 'duration_ms')), dwell: num(sp(i, 'dwell_ms')),
    profile: model.get(sp(i, 'profile')) || 'bell', accel: num(sp(i, 'accel_pct'), 25),
    mode: model.get(sp(i, 'mode')) || 'return', range: num(sp(i, 'range_deg'), 180),
  };
}

function servoCycle(s) {
  if (s.mode === 'once') return s.delay + s.dur;
  if (s.mode === 'return') return s.delay + 2 * s.dur + s.dwell;
  return 0;
}

function effectivePeriod() {
  const p = num('/scene/period_ms');
  if (p > 0) return p;
  let longest = 0;
  for (const i of servoIndices()) if (servoActive(i)) longest = Math.max(longest, servoCycle(servoParams(i)));
  return Math.max(longest, 100);
}

function targetDeg(s, tMs, period) {
  const along = (t) => s.start + (s.stop - s.start) * profilePos(s.profile, s.dur > 0 ? t / s.dur : 1, s.accel);
  const back = (t) => s.stop + (s.start - s.stop) * profilePos(s.profile, s.dur > 0 ? t / s.dur : 1, s.accel);
  // start → stop, dwell, stop → start, then hold start
  const cycleShape = (t) => {
    if (t <= 0) return s.start;
    if (t < s.dur) return along(t);
    t -= s.dur;
    if (t < s.dwell) return s.stop;
    t -= s.dwell;
    if (t < s.dur) return back(t);
    return s.start;
  };
  if (s.mode === 'once') {
    const t = tMs - s.delay;
    return t <= 0 ? s.start : t >= s.dur ? s.stop : along(t);
  }
  if (s.mode === 'pingpong') {
    const local = tMs - s.delay;
    const cyc = 2 * s.dur + 2 * s.dwell;
    if (local <= 0 || cyc <= 0) return s.start;
    return cycleShape(local % cyc);
  }
  const inCycle = period > 0 ? tMs % period : tMs;
  return cycleShape(inCycle - s.delay);
}

// Scene time in ms, interpolated between the 5 Hz status pushes.
function sceneTimeMs() {
  const extra = status.run ? Math.min(performance.now() - statusAt, 500) : 0;
  return status.t * 1000 + extra;
}

// ---------------------------------------------------------------------------
// Connection

let ws = null;
const pending = [];
let logStatusOnce = false;
let muted = false;

function connect() {
  ws = new WebSocket(`ws://${location.host}/ws`);
  ws.onopen = () => { setConn(true); loadAll(); };
  ws.onclose = () => {
    setConn(false);
    for (const p of pending.splice(0)) {
      clearTimeout(p.timer);
      p.reject(new Error('disconnected'));
    }
    setTimeout(connect, 1500);
  };
  ws.onmessage = (e) => onFrame(e.data);
}

function send(cmd, { quiet = false } = {}) {
  return new Promise((resolve, reject) => {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      toast('Not connected', true);
      reject(new Error('offline'));
      return;
    }
    // Replies are matched to requests by order. If one ever goes missing, every later
    // reply would land on the wrong request, so resynchronise by reconnecting.
    const entry = { resolve, reject };
    entry.timer = setTimeout(() => {
      toast('Device stopped answering, reconnecting…', true);
      if (ws) ws.close();  // onclose rejects everything pending, then reconnects and reloads
    }, REPLY_TIMEOUT_MS);
    pending.push(entry);
    ws.send(cmd);
    if (!quiet) logLine(cmd, 'tx');
  });
}

function realtime(c) {
  if (ws && ws.readyState === WebSocket.OPEN) ws.send(c);
  else toast('Not connected', true);
}

function onFrame(text) {
  const lines = text.split('\n').filter((l) => l.length);
  const last = lines[lines.length - 1] || '';
  const isReply = last === 'ok' || last.startsWith('error:');
  for (const l of lines) handleLine(l, isReply);
  if (isReply) {
    const p = pending.shift();
    if (p) {
      clearTimeout(p.timer);
      p.resolve({ ok: last === 'ok', lines: lines.slice(0, -1), error: last });
    }
  }
}

// isReply: part of the answer to one of our commands (info lines are not toasted then).
function handleLine(l, isReply = false) {
  if (l.startsWith('<')) {
    parseStatus(l);
    if (logStatusOnce) { logLine(l); logStatusOnce = false; }
    return;
  }
  if (l.startsWith('[POS:')) { parsePositions(l); return; }
  if (!muted) logLine(l, l.startsWith('error') || l.startsWith('[MSG:ERR') ? 'err' : '');

  if (l[0] === '$') {
    const eq = l.indexOf('=');
    if (eq > 1) model.set(l.slice(1, eq), l.slice(eq + 1));
  } else if (l.startsWith('[SERVO:')) {
    const m = /^\[SERVO:(\d+) (\S+) (.+)\]$/.exec(l);
    if (m) hwInfo[m[1]] = `${m[2]} → ${m[3]}`;
  } else if (l.startsWith('[UPDATE:')) {
    setDefaultUpdateUrl(l.slice(8, -1));
  } else if (l.startsWith('[OTA:')) {
    otaProgress(l.slice(5, -1));
  } else if (l.startsWith('[MSG:INFO: Firmware ') && l.includes(' in ota_')) {
    $('#fwVersion').textContent = l.slice(11, -1);
  } else if (muted) {
    // no toasts while (re)loading
  } else if (l.startsWith('[MSG:ERR:')) {
    const msg = l.slice(9, -1).trim();
    if (msg.startsWith('Firmware update failed')) setFwStatus(msg, true);
    toast(msg, true);
  } else if (l.startsWith('[MSG:WARN:')) {
    toast(l.slice(10, -1).trim(), true);
  } else if (l.startsWith('[MSG:INFO:')) {
    const msg = l.slice(10, -1).trim();
    if (msg.startsWith('Restart required')) $('#restart').hidden = false;
    else if (msg.startsWith('Firmware ') && msg.endsWith('restarting')) { setFwStatus(msg); toast(msg); }
    else if (!isReply) toast(msg);
  }
}

function parseStatus(l) {
  const parts = l.slice(1, -1).split('|');
  const s = { run: parts[0] === 'Run', t: 0, cyc: 0, stop: new Set(), rejoin: new Set(), test: new Set() };
  for (const p of parts.slice(1)) {
    const [k, v] = p.split(':');
    if (k === 'T') s.t = parseFloat(v);
    else if (k === 'Cyc') s.cyc = parseInt(v, 10);
    else if (k === 'Stop' || k === 'Rejoin' || k === 'Test') {
      s[k.toLowerCase()] = new Set(v.split(',').map(Number));
    }
  }
  status = s;
  statusAt = performance.now();
  updateStatusUi();
}

function parsePositions(l) {
  for (const kv of l.slice(5, -1).split(',')) {
    const [i, v] = kv.split('=');
    if (v !== undefined) positions[i] = parseFloat(v);
  }
}

async function loadAll() {
  model.clear();
  for (const k of Object.keys(hwInfo)) delete hwInfo[k];
  muted = true;  // a full listing would flood the console and raise toasts for old boot warnings
  try {
    await send('$$', { quiet: true });
    const info = await send('$I', { quiet: true });
    bootWarnings = info.lines.filter((l) => l.startsWith('[MSG:WARN:')).map((l) => l.slice(10, -1).trim());
  } catch (e) {
    return;
  } finally {
    muted = false;
  }
  renderAll();
}

// ---------------------------------------------------------------------------
// Settings binding

const bound = new Map();  // path → Set of inputs

function bind(input, path) {
  if (!bound.has(path)) bound.set(path, new Set());
  bound.get(path).add(input);
  setInputValue(input, model.get(path));
  input.addEventListener('change', () => {
    let v = input.type === 'checkbox' ? String(input.checked) : input.value.trim();
    if (input.dataset.number !== undefined) v = v.replace(',', '.');  // decimal comma (e.g. German keyboards)
    setSetting(path, v);
  });
  return input;
}

function setInputValue(input, v) {
  if (v === undefined) return;
  if (input.type === 'checkbox') input.checked = v === 'true';
  else if (input.type === 'password') input.value = '';
  else input.value = v;
}

function refreshBound(path) {
  for (const input of bound.get(path) || []) if (document.activeElement !== input) setInputValue(input, model.get(path));
}

async function setSetting(path, value) {
  let r;
  try {
    r = await send(`$${path}=${value}`);
  } catch (e) {
    refreshBound(path);
    return false;
  }
  if (r.ok) {
    model.set(path, value);
    setDirty(true);
  } else {
    await send(`$${path}`, { quiet: true }).catch(() => {});
  }
  refreshBound(path);
  onModelChanged(path);
  return r.ok;
}

function onModelChanged(path) {
  const m = /^\/servo\/(\d+)\/(\w+)$/.exec(path);
  if (m) {
    const i = Number(m[1]);
    const card = document.querySelector(`.card[data-servo="${i}"]`);
    if (card) updateCard(card, i);
    if (m[2] === 'name') renderCalibrateList();
    if (m[2] === 'range_deg') updateCalibrateRange();
  }
  renderTimeline();
}

function field(label, input, cls = '') {
  return el('div', { class: `field ${cls}` }, el('label', {}, label), input);
}

function inputFor(path, name) {
  const v = model.get(path);
  if (ENUMS[name]) return bind(el('select', {}, ENUMS[name].map((o) => el('option', { value: o }, o))), path);
  if (v === 'true' || v === 'false') return bind(el('input', { type: 'checkbox' }), path);
  if (/password/i.test(name)) return bind(el('input', { type: 'password', placeholder: 'unchanged', autocomplete: 'new-password' }), path);
  const numeric = /^-?[\d.]+$/.test(v || '') && !/^0x/i.test(v || '');
  return bind(el('input', {
    type: 'text', spellcheck: 'false', 'data-number': numeric ? '' : undefined,
    inputmode: numeric && !SIGNED_FIELDS.has(name) ? 'decimal' : undefined,
  }), path);
}

function genericField(path, label) {
  const name = path.split('/').pop();
  const input = inputFor(path, name);
  if (path === 'AP/SSID') input.placeholder = 'auto (ServoScene-XXXX)';
  return field(label || name.replace(/_/g, ' '), input, input.type === 'checkbox' ? 'check' : '');
}

// ---------------------------------------------------------------------------
// Header

function setConn(on) {
  const c = $('#conn');
  c.textContent = on ? 'online' : 'offline';
  c.classList.toggle('on', on);
}

function setDirty(d) {
  dirty = d;
  $('#btnSave').classList.toggle('dirty', d);
  $('#btnSave').textContent = d ? 'Save*' : 'Save';
}

function updateStatusUi() {
  const badge = $('#state');
  badge.textContent = status.run ? 'Run' : 'Hold';
  badge.className = `badge ${status.run ? 'run' : 'hold'}`;
  const btn = $('#btnRun');
  btn.textContent = status.run ? 'Stop' : 'Go';
  btn.className = `big ${status.run ? 'stop' : 'go'}`;
  $('#clock').textContent = `T ${status.t.toFixed(1)} s · cycle ${status.cyc}`;
  for (const card of document.querySelectorAll('.card')) updateCardStatus(card, Number(card.dataset.servo));
}

// ---------------------------------------------------------------------------
// Servo cards

function renderCards() {
  $('#cards').replaceChildren(...servoIndices().map(buildCard));
}

function angleRow(label, path, i) {
  const range = el('input', { type: 'range', min: 0, max: num(sp(i, 'range_deg'), 180), step: 0.5 });
  const box = el('input', { type: 'text', inputmode: 'decimal', 'data-number': '' });
  bind(range, path);
  bind(box, path);
  range.addEventListener('input', () => { box.value = range.value; });
  return [el('label', {}, label), range, box];
}

function buildCard(i) {
  const runBtn = el('button', { class: 'run-toggle', title: 'Stop / go this servo' });
  runBtn.addEventListener('click', () => {
    send(`${status.stop.has(i) ? '$Servo/Go' : '$Servo/Stop'}=${i}`);
  });
  const name = bind(el('input', { class: 'name', placeholder: `Servo ${i}`, maxlength: 15 }), sp(i, 'name'));
  const enable = bind(el('input', { type: 'checkbox', title: 'Enabled' }), sp(i, 'enable'));
  const pinTag = el('span', { class: 'idx' });
  const stateTag = el('span', { class: 'tag' });
  const canvas = el('canvas', { class: 'plot' });

  const accel = genericField(sp(i, 'accel_pct'), 'accel %');
  const card = el('div', { class: 'card', 'data-servo': i },
    el('div', { class: 'card-head' }, runBtn, name, stateTag, enable),
    el('div', { class: 'card-head' }, pinTag),
    canvas,
    el('div', { class: 'angles' }, angleRow('Start', sp(i, 'start_deg'), i), angleRow('Stop', sp(i, 'stop_deg'), i)),
    el('div', { class: 'fields' },
      genericField(sp(i, 'duration_ms'), 'duration ms'),
      genericField(sp(i, 'delay_ms'), 'delay ms'),
      genericField(sp(i, 'dwell_ms'), 'dwell ms'),
      genericField(sp(i, 'profile')),
      accel,
      genericField(sp(i, 'mode'))));
  card._refs = { runBtn, pinTag, stateTag, canvas, accel };
  updateCard(card, i);
  return card;
}

function updateCard(card, i) {
  const r = card._refs;
  r.pinTag.textContent = `#${i} · ${model.get(sp(i, 'pin'))}${hwInfo[i] ? ' · ' + hwInfo[i].split('→ ')[1] : ''}`;
  r.accel.hidden = model.get(sp(i, 'profile')) !== 'trap';
  card.classList.toggle('disabled', !servoActive(i));
  for (const inp of card.querySelectorAll('input[type=range]')) inp.max = num(sp(i, 'range_deg'), 180);
  updateCardStatus(card, i);
}

function updateCardStatus(card, i) {
  const r = card._refs;
  if (!r) return;
  const stopped = status.stop.has(i);
  r.runBtn.textContent = stopped ? '▶' : '⏸';
  r.runBtn.className = `run-toggle ${stopped ? 'stopped' : 'running'}`;
  r.runBtn.title = stopped ? 'Go: blend back into the scene' : 'Stop this servo';
  card.classList.toggle('stopped', stopped);
  let tag = '';
  if (status.test.has(i)) tag = 'testing';
  else if (stopped) tag = 'stopped';
  else if (status.rejoin.has(i)) tag = 'rejoining';
  r.stateTag.textContent = tag;
  r.stateTag.hidden = !tag;
  r.stateTag.className = `tag ${tag === 'stopped' || tag === 'testing' ? 'warn' : ''}`;
}

// The time window a servo's plot shows. Return/once servos repeat with the scene cycle;
// a pingpong servo repeats on its own cycle (2·duration + 2·dwell, after its delay), which
// in general does not divide the scene period, so it gets a window of exactly one own cycle.
function plotWindow(s, period) {
  const own = 2 * s.dur + 2 * s.dwell;
  if (s.mode === 'pingpong' && own > 0) {
    return {
      span: own,
      at: (tMs) => (tMs <= s.delay ? 0 : (tMs - s.delay) % own),
      curve: (tau) => targetDeg(s, s.delay + own + tau, period),  // skip the initial delay
    };
  }
  return {
    span: period,
    at: (tMs) => (s.mode === 'once' ? Math.min(tMs, period) : tMs % period),
    curve: (tau) => targetDeg(s, tau, period),
  };
}

// A servo that is simply following the choreography outputs exactly its target, so its
// dot is drawn on the curve at the (interpolated) cursor. While stopped, rejoining or
// testing it shows the last reported position at the time it was measured, so the lag
// behind the choreography stays visible.
const followsTarget = (i) => !status.stop.has(i) && !status.rejoin.has(i) && !status.test.has(i);

// Angle over one cycle, cursor at the current scene time, dot at the servo's output.
function drawPlot(canvas, i, tMs, period) {
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth, h = canvas.clientHeight;
  if (!w) return;
  if (canvas.width !== Math.round(w * dpr)) { canvas.width = Math.round(w * dpr); canvas.height = Math.round(h * dpr); }
  const ctx = canvas.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);
  const css = getComputedStyle(document.documentElement);
  const s = servoParams(i);
  const pad = 4;
  const y = (deg) => h - pad - (deg / s.range) * (h - 2 * pad);
  const win = plotWindow(s, period);
  const x = (tau) => (tau / win.span) * w;

  ctx.fillStyle = css.getPropertyValue('--track');
  ctx.fillRect(0, 0, w, h);
  ctx.strokeStyle = css.getPropertyValue('--move');
  ctx.lineWidth = 2;
  ctx.beginPath();
  const steps = Math.min(240, Math.round(w));
  for (let k = 0; k <= steps; k++) {
    const tau = (k / steps) * win.span;
    if (k === 0) ctx.moveTo(x(tau), y(win.curve(tau))); else ctx.lineTo(x(tau), y(win.curve(tau)));
  }
  ctx.stroke();

  const cursor = x(win.at(tMs));
  ctx.strokeStyle = css.getPropertyValue('--stop');
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(cursor, 0);
  ctx.lineTo(cursor, h);
  ctx.stroke();

  if (positions[i] === undefined) return;
  const live = followsTarget(i);
  const dotX = live ? cursor : x(win.at(status.t * 1000));
  const dotDeg = live ? targetDeg(s, tMs, period) : positions[i];
  ctx.fillStyle = css.getPropertyValue('--text');
  ctx.beginPath();
  ctx.arc(dotX, y(dotDeg), 4, 0, 2 * Math.PI);
  ctx.fill();
}

// ---------------------------------------------------------------------------
// Timeline

function renderTimeline() {
  const host = $('#timeline');
  if (host.closest('.tab').hidden) return;
  const idx = servoIndices().filter(servoActive);
  const period = effectivePeriod();
  const cycle = Math.floor(sceneTimeMs() / period);
  const w0 = cycle * period;  // start of the scene cycle being shown
  // Drawn at the real pixel width so labels stay readable on phones.
  const W = Math.max(host.clientWidth, 320), label = W < 600 ? 72 : 110, rowH = 22, top = 20;
  const H = top + idx.length * rowH + 6;
  const sx = (t) => label + (Math.min(t, period) / period) * (W - label - 6);
  const ns = 'http://www.w3.org/2000/svg';
  const svg = document.createElementNS(ns, 'svg');
  svg.setAttribute('viewBox', `0 0 ${W} ${H}`);
  const add = (tag, attrs, text) => {
    const e = document.createElementNS(ns, tag);
    for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
    if (text !== undefined) e.textContent = text;
    svg.append(e);
    return e;
  };
  const ticks = W < 600 ? 4 : 10;
  for (let k = 0; k <= ticks; k++) {
    const t = (k / ticks) * period;
    add('line', { x1: sx(t), x2: sx(t), y1: top - 4, y2: H, class: 'grid' });
    add('text', { x: sx(t), y: 12, 'text-anchor': k === ticks ? 'end' : 'middle' }, `${(t / 1000).toFixed(1)}s`);
  }
  idx.forEach((i, row) => {
    const s = servoParams(i);
    const y = top + row * rowH;
    const g = status.stop.has(i) ? 'stopped' : '';
    add('text', { x: 4, y: y + 15 }, servoName(i).slice(0, W < 600 ? 10 : 16));
    add('rect', { x: label, y: y + 3, width: W - label - 6, height: rowH - 6, class: `track ${g}` });
    // Bars in absolute scene time, clipped to the window [w0, w0 + period).
    const bar = (t0, t1, cls) => {
      const a = Math.max(t0 - w0, 0), b = Math.min(t1 - w0, period);
      if (b <= a) return;
      add('rect', { x: sx(a), y: y + 3, width: Math.max(1, sx(b) - sx(a)), height: rowH - 6, rx: 3, class: `${cls} ${g}` });
    };
    const own = 2 * s.dur + 2 * s.dwell;
    if (s.mode === 'pingpong' && own > 0) {
      // Own cycle, not tied to the scene period: draw the cycles that overlap this window.
      for (let k = Math.max(0, Math.floor((w0 - s.delay) / own)); s.delay + k * own < w0 + period; k++) {
        const t = s.delay + k * own;
        bar(t, t + s.dur, 'move');
        bar(t + s.dur, t + s.dur + s.dwell, 'dwell');
        bar(t + s.dur + s.dwell, t + 2 * s.dur + s.dwell, 'move');
      }
    } else if (s.mode === 'once') {
      bar(s.delay, s.delay + s.dur, 'move');  // only in the first cycle; afterwards it holds
    } else {
      const c = w0;  // return mode repeats every scene cycle
      bar(c + s.delay, c + s.delay + s.dur, 'move');
      bar(c + s.delay + s.dur, c + s.delay + s.dur + s.dwell, 'dwell');
      bar(c + s.delay + s.dur + s.dwell, c + s.delay + 2 * s.dur + s.dwell, 'move');
    }
  });
  const cursor = add('line', { x1: label, x2: label, y1: top - 4, y2: H, class: 'cursor' });
  host._cursor = { cursor, sx, period, cycle };
  host.replaceChildren(svg);
}

function updateTimelineCursor(tMs) {
  let c = $('#timeline')._cursor;
  if (!c) return;
  // New scene cycle (or period changed): pingpong bars sit elsewhere in the next window.
  if (Math.floor(tMs / c.period) !== c.cycle || c.period !== effectivePeriod()) {
    renderTimeline();
    c = $('#timeline')._cursor;
  }
  const x = c.sx(tMs % c.period);
  c.cursor.setAttribute('x1', x);
  c.cursor.setAttribute('x2', x);
}

// ---------------------------------------------------------------------------
// Calibrate

let calServo = null;
let testInFlight = false;
let testQueued = null;

function renderCalibrateList() {
  const sel = $('#calServo');
  const keep = calServo;
  sel.replaceChildren(...servoIndices().map((i) => el('option', { value: i }, `${i}: ${servoName(i)}`)));
  if (keep !== null && servoIndices().includes(keep)) sel.value = keep;
  calServo = sel.value === '' ? null : Number(sel.value);
}

function updateCalibrateRange() {
  if (calServo === null) return;
  $('#calDeg').max = num(sp(calServo, 'range_deg'), 180);
}

function renderCalibrate() {
  renderCalibrateList();
  const fields = $('#calFields');
  if (calServo === null) { fields.replaceChildren(); return; }
  const i = calServo;
  $('#calHw').textContent = hwInfo[i] ? `Output: ${hwInfo[i]}` : '';
  updateCalibrateRange();
  if (positions[i] !== undefined) {
    $('#calDeg').value = positions[i];
    $('#calDegOut').textContent = `${positions[i].toFixed(1)}°`;
  }
  const remove = el('button', { class: 'danger' }, 'Remove servo');
  remove.addEventListener('click', async () => {
    if (!confirm(`Remove servo ${i} (${servoName(i)})? Its pin is freed after a restart.`)) return;
    if (await setSetting(sp(i, 'pin'), 'none')) { calServo = null; renderAll(); }
  });
  fields.replaceChildren(
    ...['min_us', 'max_us', 'range_deg', 'trim_us', 'max_dps', 'invert', 'pin', 'driver'].map((f) => genericField(sp(i, f), f.replace(/_/g, ' '))),
    field('', remove));
}

function sendTest(deg) {
  if (calServo === null) return;
  if (testInFlight) { testQueued = deg; return; }
  testInFlight = true;
  send(`$Servo/Test=${calServo},${deg}`, { quiet: true })
    .catch(() => {})
    .finally(() => {
      testInFlight = false;
      if (testQueued !== null) { const d = testQueued; testQueued = null; sendTest(d); }
    });
}

// ---------------------------------------------------------------------------
// Settings tab

const GROUP_TITLES = { scene: 'Scene', pwm: 'PWM frequencies', i2c: 'I²C bus', button: 'Stop/Go button' };

function renderSettings() {
  const groups = new Map();
  for (const path of model.keys()) {
    if (path.startsWith('/servo/')) continue;
    let key;
    if (path.startsWith('/pca/')) key = `PCA9685 board ${path.split('/')[2]}`;
    else if (path.startsWith('/')) key = GROUP_TITLES[path.split('/')[1]] || path.split('/')[1];
    else key = 'WiFi access point';
    if (!groups.has(key)) groups.set(key, []);
    groups.get(key).push(path);
  }
  const panels = [];
  if (bootWarnings.length) {
    panels.push(el('div', { class: 'panel' }, el('h2', {}, 'Startup problems'),
      el('pre', { class: 'info' }, bootWarnings.join('\n'))));
  }
  for (const [title, paths] of groups) {
    panels.push(el('div', { class: 'panel' }, el('h2', {}, title),
      el('div', { class: 'fields' }, paths.map((p) => genericField(p, p.startsWith('/') ? undefined : p)))));
  }
  $('#settingsGroups').replaceChildren(...panels);
}

// ---------------------------------------------------------------------------
// Console

const MAX_LOG = 600;
const history = [];
let histPos = 0;

function logLine(text, cls = '') {
  const log = $('#log');
  const atBottom = log.scrollTop + log.clientHeight >= log.scrollHeight - 4;
  log.append(el('div', { class: cls }, cls === 'tx' ? `> ${text}` : text));
  while (log.childElementCount > MAX_LOG) log.firstChild.remove();
  if (atBottom) log.scrollTop = log.scrollHeight;
}

function consoleSend(line) {
  if (!line) return;
  history.push(line);
  histPos = history.length;
  if (line.length === 1 && '!~?'.includes(line)) {
    logLine(line, 'tx');
    if (line === '?') logStatusOnce = true;
    realtime(line);
    return;
  }
  send(line).then((r) => { if (r.ok && /^\$(\/|AP\/|Hostname|RST|CS|Save|Config)/i.test(line)) loadAll(); }).catch(() => {});
}

// ---------------------------------------------------------------------------
// Firmware update

const UPDATE_PORT = 9999;
let fwHostEdited = false;

const updateUrlFor = (host) => `http://${host}:${UPDATE_PORT}/firmware.bin`;

// [UPDATE:http://<first client>:9999/firmware.bin] from $I prefills the server address.
function setDefaultUpdateUrl(url) {
  const m = /^https?:\/\/([^:/]+)/.exec(url);
  if (m && !fwHostEdited) $('#fwHost').value = m[1];
  showFwUrl();
}

function showFwUrl() {
  const host = $('#fwHost').value.trim().replace(/,/g, '.');
  $('#fwUrl').textContent = updateUrlFor(host || '…');
}

function setFwStatus(text, err = false) {
  const s = $('#fwStatus');
  s.textContent = text;
  s.classList.toggle('err', err);
  if (err) $('#fwProgress').hidden = true;
}

function setFwProgress(pct) {
  const p = $('#fwProgress');
  p.hidden = false;
  p.value = pct;
}

// [OTA:start <url>] / [OTA:download 40%]
function otaProgress(text) {
  const m = /^download (\d+)%$/.exec(text);
  if (m) {
    setFwProgress(Number(m[1]));
    setFwStatus(`Downloading and flashing… ${m[1]}%`);
  } else if (text.startsWith('start ')) {
    setFwProgress(0);
    setFwStatus(`Fetching ${text.slice(6)}…`);
  }
}

function confirmFirmwareUpdate(what) {
  const unsaved = dirty ? '\n\nUnsaved setting changes will be lost when it restarts.' : '';
  return confirm(`Install firmware from ${what}? The ESP restarts afterwards.${unsaved}`);
}

async function pullUpdate() {
  const host = $('#fwHost').value.trim().replace(/,/g, '.');
  if (!/^[\w.-]+$/.test(host)) { setFwStatus('Enter the IP address of the computer serving the firmware', true); return; }
  if (!confirmFirmwareUpdate(updateUrlFor(host))) return;
  setFwStatus('Starting…');
  const r = await send(`$Firmware/Update=${updateUrlFor(host)}`).catch(() => null);
  if (!r) setFwStatus('Not connected', true);
  else if (!r.ok) setFwStatus(r.lines.filter((l) => l.startsWith('[MSG:ERR:')).map((l) => l.slice(9, -1).trim()).join(' ') || r.error, true);
}

async function uploadFirmware(file) {
  const head = new Uint8Array(await file.slice(0, 1).arrayBuffer());
  if (head[0] !== 0xE9) { setFwStatus(`${file.name} is not an ESP32 firmware image`, true); return; }
  if (!confirmFirmwareUpdate(file.name)) return;
  setFwProgress(0);
  setFwStatus(`Uploading ${file.name}…`);
  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/update');
  xhr.upload.onprogress = (e) => {
    if (!e.lengthComputable) return;
    const pct = Math.round((e.loaded / e.total) * 100);
    setFwProgress(pct);
    setFwStatus(pct < 100 ? `Uploading… ${pct}%` : 'Verifying signature and flashing…');
  };
  xhr.onload = () => setFwStatus(xhr.responseText || `HTTP ${xhr.status}`, xhr.status !== 200);
  xhr.onerror = () => setFwStatus('Upload failed (connection lost)', true);
  xhr.send(file);
}

// ---------------------------------------------------------------------------
// UI glue

let toastTimer = null;
function toast(text, err = false) {
  const t = $('#toast');
  t.textContent = text;
  t.className = `toast ${err ? 'err' : ''}`;
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.hidden = true; }, err ? 5000 : 2500);
}

function renderAll() {
  bound.clear();
  renderCards();
  renderCalibrate();
  renderSettings();
  renderTimeline();
  updateStatusUi();
}

function showTab(name) {
  for (const b of document.querySelectorAll('.tabs button')) b.setAttribute('aria-selected', b.dataset.tab === name);
  for (const s of document.querySelectorAll('.tab')) s.hidden = s.id !== `tab-${name}`;
  if (name === 'timeline') renderTimeline();
  if (name === 'calibrate') renderCalibrate();
  try { localStorage.setItem('tab', name); } catch (e) { /* storage unavailable */ }
}

async function saveConfig() {
  const r = await send('$CS').catch(() => null);
  if (r && r.ok) { setDirty(false); toast('Saved to flash'); }
  return r && r.ok;
}

function addServo() {
  const used = new Set(servoIndices());
  let free = -1;
  for (let i = 0; i < MAX_SERVOS; i++) if (!used.has(i)) { free = i; break; }
  if (free < 0) { toast('All 64 servo slots are in use', true); return; }
  const pin = prompt(`Pin for servo ${free} (gpio.N or pca.B.C):`, 'pca.0.0');
  if (!pin) return;
  (async () => {
    if (!(await setSetting(sp(free, 'pin'), pin.trim()))) return;
    await setSetting(sp(free, 'enable'), 'true');
    await send(`$/servo/${free}`, { quiet: true });  // fetch all fields of the new slot
    renderAll();
  })();
}

function animate() {
  const tMs = sceneTimeMs();
  if (!$('#tab-servos').hidden) {
    const period = effectivePeriod();
    for (const card of document.querySelectorAll('.card')) {
      drawPlot(card._refs.canvas, Number(card.dataset.servo), tMs, period);
    }
  }
  if (!$('#tab-timeline').hidden) updateTimelineCursor(tMs);
  if (!$('#tab-calibrate').hidden && calServo !== null && positions[calServo] !== undefined &&
      document.activeElement !== $('#calDeg')) {
    $('#calDegOut').textContent = `${positions[calServo].toFixed(1)}°`;
  }
  setTimeout(() => requestAnimationFrame(animate), 50);
}

function init() {
  for (const b of document.querySelectorAll('.tabs button')) b.addEventListener('click', () => showTab(b.dataset.tab));
  let tab = 'servos';
  try { tab = localStorage.getItem('tab') || tab; } catch (e) { /* storage unavailable */ }
  showTab(tab);

  $('#btnRun').addEventListener('click', () => realtime(status.run ? '!' : '~'));
  $('#btnReset').addEventListener('click', () => send('$Scene/Reset'));
  $('#btnAllGo').addEventListener('click', () => send('$Servo/Go=*'));
  $('#btnAllStop').addEventListener('click', () => send('$Servo/Stop=*'));
  $('#btnSave').addEventListener('click', saveConfig);
  $('#btnSaveRestart').addEventListener('click', async () => {
    if (await saveConfig()) { await send('$Bye'); toast('Restarting…'); $('#restart').hidden = true; }
  });
  $('#btnAdd').addEventListener('click', addServo);

  $('#calServo').addEventListener('change', (e) => { calServo = Number(e.target.value); renderCalibrate(); });
  $('#calDeg').addEventListener('input', (e) => {
    $('#calDegOut').textContent = `${parseFloat(e.target.value).toFixed(1)}°`;
    sendTest(e.target.value);
  });
  $('#calCenter').addEventListener('click', () => {
    if (calServo === null) return;
    const mid = num(sp(calServo, 'range_deg'), 180) / 2;
    $('#calDeg').value = mid;
    sendTest(mid);
  });
  $('#calRelease').addEventListener('click', () => { if (calServo !== null) send(`$Servo/Release=${calServo}`); });

  $('#btnInfo').addEventListener('click', async () => {
    const r = await send('$I').catch(() => null);
    if (r) $('#infoOut').textContent = r.lines.join('\n');
  });
  $('#btnDefaults').addEventListener('click', async () => {
    if (!confirm('Restore factory defaults and erase the saved configuration?')) return;
    const r = await send('$RST=*').catch(() => null);
    if (r && r.ok) { setDirty(false); $('#restart').hidden = false; loadAll(); }
  });
  $('#btnRestart').addEventListener('click', async () => {
    if (dirty && !confirm('Unsaved changes will be lost. Restart anyway?')) return;
    await send('$Bye').catch(() => {});
    toast('Restarting…');
  });
  $('#restoreFile').addEventListener('change', async (e) => {
    const file = e.target.files[0];
    if (!file) return;
    const res = await fetch('/config.txt', { method: 'POST', body: await file.text() }).catch(() => null);
    e.target.value = '';
    if (!res || !res.ok) { toast('Restore failed', true); return; }
    $('#infoOut').textContent = await res.text();
    setDirty(true);
    toast('Config restored — press Save to keep it');
    loadAll();
  });

  $('#fwHost').addEventListener('input', () => { fwHostEdited = true; showFwUrl(); });
  $('#btnFwPull').addEventListener('click', pullUpdate);
  $('#fwFile').addEventListener('change', (e) => {
    const file = e.target.files[0];
    e.target.value = '';
    if (file) uploadFirmware(file);
  });
  showFwUrl();

  $('#consoleForm').addEventListener('submit', (e) => {
    e.preventDefault();
    const input = $('#consoleLine');
    consoleSend(input.value.trim());
    input.value = '';
  });
  $('#consoleLine').addEventListener('keydown', (e) => {
    if (e.key === 'ArrowUp' && histPos > 0) { e.target.value = history[--histPos]; e.preventDefault(); }
    if (e.key === 'ArrowDown') { histPos = Math.min(history.length, histPos + 1); e.target.value = history[histPos] || ''; e.preventDefault(); }
  });
  for (const b of document.querySelectorAll('[data-cmd]')) b.addEventListener('click', () => consoleSend(b.dataset.cmd));
  $('#btnClearLog').addEventListener('click', () => $('#log').replaceChildren());

  window.addEventListener('beforeunload', (e) => { if (dirty) { e.preventDefault(); e.returnValue = ''; } });
  let resizeTimer = null;
  window.addEventListener('resize', () => { clearTimeout(resizeTimer); resizeTimer = setTimeout(renderTimeline, 150); });

  connect();
  animate();
}

init();
