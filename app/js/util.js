// Shared helpers: constants, DOM builder, event bus, knob widget, menus, dialogs.
export const PPQ = 96, STEP = 24;
// Bar and beat lengths follow the project's time signature. These are live bindings: every importer sees the current value.
export let BAR = 384, BEAT = 96, SIG = [4, 4];
export function setSig(n, d) { SIG = [n, d]; BEAT = PPQ * 4 / d; BAR = n * BEAT; }
export const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
// Meter position 0..1 for a linear peak level: -60 dB at the bottom, 0 dB at the top.
export const meterPos = v => (v <= 0.001 ? 0 : clamp((20 * Math.log10(v) + 60) / 60, 0, 1));
export const uid = () => Math.random().toString(36).slice(2, 10);
export const NOTES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
export const noteName = n => NOTES[((n % 12) + 12) % 12] + (Math.floor(n / 12) - 1);
export const mtof = n => 440 * 2 ** ((n - 69) / 12);
export const isBlack = n => [1, 3, 6, 8, 10].includes(((n % 12) + 12) % 12);
export const SCALES = {
  chromatic: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11], major: [0, 2, 4, 5, 7, 9, 11], minor: [0, 2, 3, 5, 7, 8, 10],
  dorian: [0, 2, 3, 5, 7, 9, 10], phrygian: [0, 1, 3, 5, 7, 8, 10], lydian: [0, 2, 4, 6, 7, 9, 11], mixolydian: [0, 2, 4, 5, 7, 9, 10],
  'harmonic minor': [0, 2, 3, 5, 7, 8, 11], 'melodic minor': [0, 2, 3, 5, 7, 9, 11], 'major pentatonic': [0, 2, 4, 7, 9],
  'minor pentatonic': [0, 3, 5, 7, 10], blues: [0, 3, 5, 6, 7, 10],
};

const L = {};
export const bus = {
  on(e, f) { (L[e] ||= []).push(f); },
  emit(e, d) { (L[e] || []).forEach(f => f(d)); },
};

export function h(tag, a, ...kids) {
  const el = document.createElement(tag);
  if (a) for (const k in a) {
    const v = a[k];
    if (v == null || v === false) continue;
    if (k === 'class') el.className = v;
    else if (k === 'style') el.style.cssText = v;
    else if (k.startsWith('on')) el.addEventListener(k.slice(2), v);
    else if (k in el) el[k] = v;
    else el.setAttribute(k, v);
  }
  el.append(...kids.flat(3).filter(x => x != null && x !== false));
  return el;
}

export function sel(opts, value, on, attrs) {
  const s = h('select', attrs, opts.map(o => { const [v, l] = Array.isArray(o) ? o : [o, o]; return h('option', { value: String(v) }, String(l)); }));
  s.value = String(value);
  s.addEventListener('change', () => on(s.value));
  return s;
}

const ICONS = {
  play: ['M8 5v14l11-7z', 1], stop: ['M6 6h12v12H6z', 1], rec: ['M12 6a6 6 0 1 0 0 12 6 6 0 0 0 0-12z', 1],
  metro: ['M9 3h6l4 18H5zM12 16l5-10'], undo: ['M8 5L3 10l5 5M3 10h11a5 5 0 0 1 0 10h-4'], redo: ['M16 5l5 5-5 5M21 10H10a5 5 0 0 0 0 10h4'],
  keys: ['M3 5h18v14H3zM8 5v8M12 5v8M16 5v8M9 13v6M15 13v6'], menu: ['M4 6h16M4 12h16M4 18h16'], plus: ['M12 5v14M5 12h14'],
  draw: ['M4 20l1-4L16 5l3 3L8 19z'], select: ['M4 4h4M12 4h4M20 4v4M20 12v4M20 20h-4M12 20H8M4 20v-4M4 12V8'],
  slice: ['M5 19L19 5M14 5h5v5'], erase: ['M7 20h11M5 13l7-8 7 6-6 8H9z'],
  pan: ['M12 3v18M3 12h18M9 6l3-3 3 3M9 18l3 3 3-3M6 9l-3 3 3 3M18 9l3 3-3 3'],
  midi: ['M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zM8 13h.01M16 13h.01M12 16h.01M9 9h.01M15 9h.01'],
  rack: ['M4 6h3v3H4zM10.5 6h3v3h-3zM17 6h3v3h-3zM4 15h3v3H4zM10.5 15h3v3h-3zM17 15h3v3h-3z'],
  roll: ['M3 5h18v14H3zM8 5v14M11 9h6M13 15h6'], song: ['M3 6h8M13 6h8M3 12h12M17 12h4M3 18h5M10 18h11'],
  mixer: ['M6 3v18M12 3v18M18 3v18M4 8h4M10 15h4M16 10h4'], inst: ['M2 12c2-8 4-8 6 0s4 8 6 0 4-8 6 0'],
  lab: ['M12 3a3 3 0 0 0-3 3v6a3 3 0 0 0 6 0V6a3 3 0 0 0-3-3zM6 11a6 6 0 0 0 12 0M12 17v4'],
  lib: ['M3 6h6l2 2h10v11H3z'], design: ['M12 3v4M12 17v4M3 12h4M17 12h4M6 6l2.5 2.5M15.5 15.5L18 18M18 6l-2.5 2.5M8.5 15.5L6 18'], set: ['M4 7h10M18 7h2M4 17h2M10 17h10M16 5v4M8 15v4'],
};
export function icon(name) {
  const [d, fill] = ICONS[name] || ICONS.plus;
  const s = h('span', { class: 'ico' });
  s.innerHTML = `<svg viewBox="0 0 24 24" width="18" height="18" fill="${fill ? 'currentColor' : 'none'}" stroke="${fill ? 'none' : 'currentColor'}" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="${d}"/></svg>`;
  return s;
}
export const btn = (label, fn, cls = '', title) => h('button', { class: 'btn ' + cls, title, onclick: fn }, label);

// Rotary control. Drag vertically (Shift = fine), wheel to nudge, double-click to reset.
export function knob(o) {
  const { label = '', min = 0, max = 1, def, curve, step, bi, size, onInput, onChange } = o;
  let v = o.value ?? min, t, wt;
  const toN = x => (curve === 'exp' ? Math.log(x / min) / Math.log(max / min) : (x - min) / (max - min));
  const fromN = n => {
    let x = curve === 'exp' ? min * (max / min) ** n : min + n * (max - min);
    if (step) x = Math.round(x / step) * step;
    return clamp(x, min, max);
  };
  const fmt = o.fmt || (x => (step >= 1 ? String(Math.round(x)) : Math.abs(x) >= 100 ? x.toFixed(0) : Math.abs(x) >= 10 ? x.toFixed(1) : x.toFixed(2)));
  const lab = h('span', null, label);
  const el = h('div', { class: 'knob ' + (size || ''), title: label }, h('div', { class: 'dial' }, h('i')), lab);
  const paint = show => {
    const n = clamp(toN(v), 0, 1);
    el.style.setProperty('--p', n);
    el.style.setProperty('--s', bi ? Math.min(n, 0.5) : 0);
    el.style.setProperty('--e', bi ? Math.max(n, 0.5) : n);
    if (show) {
      lab.textContent = fmt(v); lab.classList.add('val');
      clearTimeout(t); t = setTimeout(() => { lab.textContent = label; lab.classList.remove('val'); }, 900);
    }
  };
  const setN = n => { const nv = fromN(clamp(n, 0, 1)); if (nv === v) return; v = nv; paint(true); onInput && onInput(v); };
  el.addEventListener('pointerdown', e => {
    if (e.button) return;
    e.preventDefault(); el.setPointerCapture(e.pointerId);
    const y0 = e.clientY, n0 = toN(v); let moved = false;
    const mv = ev => { moved = true; setN(n0 + (y0 - ev.clientY) / (ev.shiftKey ? 900 : 170)); };
    const up = () => {
      el.removeEventListener('pointermove', mv); el.removeEventListener('pointerup', up); el.removeEventListener('pointercancel', up);
      if (moved) onChange && onChange(v); else paint(true);
    };
    el.addEventListener('pointermove', mv); el.addEventListener('pointerup', up); el.addEventListener('pointercancel', up);
  });
  el.addEventListener('dblclick', () => { if (def != null) { v = def; paint(true); onInput && onInput(v); onChange && onChange(v); } });
  el.addEventListener('wheel', e => {
    e.preventDefault();
    setN(toN(v) + (e.deltaY < 0 ? 1 : -1) * (e.shiftKey ? 0.005 : 0.03));
    clearTimeout(wt); wt = setTimeout(() => onChange && onChange(v), 400);
  }, { passive: false });
  el.set = x => { v = x; paint(); };
  // Right-click: reset, type a value, copy / paste, and (for controls with a `link` target: automation, MIDI controller).
  const setV = x => { v = clamp(x, min, max); paint(true); onInput && onInput(v); onChange && onChange(v); };
  el.addEventListener('contextmenu', e => {
    e.preventDefault(); e.stopPropagation();
    menu(e.clientX, e.clientY, [{ head: label + ' · ' + fmt(v) },
      { label: 'Reset', disabled: def == null, fn: () => setV(def) },
      '-', { head: 'Value' },
      { label: 'Type in value…', fn: async () => { const t = await ask(label + ' (' + fmt(min) + ' to ' + fmt(max) + ')', String(+v.toFixed(4))); if (t != null && t !== '' && isFinite(+t)) setV(+t); } },
      { label: 'Copy value', fn: () => { knobClip = v; } }, { label: 'Paste value', disabled: knobClip == null, fn: () => setV(knobClip) },
      ...(o.link && knobHooks.items ? knobHooks.items(o.link, v) : [])]);
  });
  if (o.link) el.dataset.link = o.link;   // MIDI-linked controls find their knob through this
  paint();
  return el;
}
let knobClip = null;
// Filled in by the app (main.js): extra right-click items for knobs with a `link` target (automation, MIDI controller).
export const knobHooks = { items: null };

// Menus. An item: { label, fn, hint, on, disabled } · { head } · '-' · { label, sub: [items] } for a sub-menu that opens
// beside it on hover or click.
let curMenu = null;
const offMenu = e => { let m = curMenu; while (m) { if (m.contains(e.target)) return; m = m.child; } closeMenu(); };
export function closeMenu() { let m = curMenu; while (m) { m.remove(); m = m.child; } if (curMenu) removeEventListener('pointerdown', offMenu, true); curMenu = null; }
function buildMenu(x, y, items, parent) {
  const m = h('div', { class: 'menu' });
  const closeChild = () => { let c = m.child; while (c) { c.remove(); c = c.child; } m.child = null; };
  for (const it of items) {
    if (!it) continue;
    if (it === '-') { m.append(h('hr')); continue; }
    if (it.head) { m.append(h('div', { class: 'mhead' }, it.head)); continue; }
    if (it.sub) {
      const b = h('button', { class: 'msub', disabled: !!it.disabled }, it.label, h('small', null, '›'));
      const open = () => { if (m.child && m.child.owner === b) return; closeChild(); const r = b.getBoundingClientRect(); m.child = buildMenu(r.right - 2, r.top - 4, it.sub, m); m.child.owner = b; };
      b.addEventListener('mouseenter', open); b.addEventListener('click', open);
      m.append(b); continue;
    }
    const b = h('button', { class: it.on ? 'on' : '', disabled: !!it.disabled, onclick: () => { closeMenu(); it.fn && it.fn(); } }, it.label, it.hint && h('small', null, it.hint));
    b.addEventListener('mouseenter', closeChild);
    m.append(b);
  }
  document.body.append(m);
  const r = m.getBoundingClientRect();
  const left = parent && x + r.width > innerWidth - 4 ? parent.getBoundingClientRect().left - r.width + 2 : x;
  m.style.left = clamp(left, 4, Math.max(4, innerWidth - r.width - 4)) + 'px';
  m.style.top = clamp(y, 4, Math.max(4, innerHeight - r.height - 4)) + 'px';
  return m;
}
export function menu(x, y, items) {
  closeMenu();
  curMenu = buildMenu(x, y, items, null);
  setTimeout(() => addEventListener('pointerdown', offMenu, true));
}
export const menuAt = (el, items) => { const r = el.getBoundingClientRect(); menu(r.left, r.bottom + 2, items); };

export function modal(title, body, btns = [{ label: 'Close' }]) {
  const close = () => ov.remove();
  const ov = h('div', { class: 'overlay', onpointerdown: e => { if (e.target === ov) close(); } },
    h('div', { class: 'modal' }, h('h3', null, title), h('div', { class: 'mbody' }, body),
      h('div', { class: 'mbtns' }, btns.map(b => h('button', { class: 'btn ' + (b.primary ? 'primary' : ''), onclick: () => { if (!b.fn || b.fn() !== false) close(); } }, b.label)))));
  document.body.append(ov);
  return close;
}
export function ask(title, value = '') {
  return new Promise(res => {
    const inp = h('input', { class: 'txt', value, onkeydown: e => { if (e.key === 'Enter') { close(); res(inp.value); } e.stopPropagation(); } });
    const close = modal(title, inp, [{ label: 'Cancel', fn: () => res(null) }, { label: 'OK', primary: true, fn: () => res(inp.value) }]);
    setTimeout(() => { inp.focus(); inp.select(); });
  });
}
let toastT;
export function toast(msg, ms = 2600) {
  let t = document.getElementById('toast');
  if (!t) document.body.append(t = h('div', { id: 'toast' }));
  t.textContent = msg; t.classList.add('show');
  clearTimeout(toastT); toastT = setTimeout(() => t.classList.remove('show'), ms);
}
export function pickFile(accept, multiple) {
  return new Promise(res => {
    const i = h('input', { type: 'file', accept, multiple: !!multiple, style: 'display:none', onchange: () => { res([...i.files]); i.remove(); } });
    document.body.append(i); i.click();
  });
}
export function download(blob, name) {
  const a = h('a', { href: URL.createObjectURL(blob), download: name });
  document.body.append(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 5000);
}
export const cssVar = n => getComputedStyle(document.documentElement).getPropertyValue(n).trim();

export function drawWave(cv, data, color) {
  const dpr = devicePixelRatio || 1, w = cv.clientWidth || 300, hh = cv.clientHeight || 60;
  cv.width = w * dpr; cv.height = hh * dpr;
  const g = cv.getContext('2d'); g.scale(dpr, dpr); g.clearRect(0, 0, w, hh);
  if (!data) return;
  g.fillStyle = color;
  const n = data.length, st = n / w;
  for (let x = 0; x < w; x++) {
    let lo = 1, hi = -1;
    const a = Math.floor(x * st), b = Math.min(n, Math.floor((x + 1) * st) + 1), inc = Math.max(1, Math.floor((b - a) / 64));
    for (let i = a; i < b; i += inc) { const v = data[i]; if (v < lo) lo = v; if (v > hi) hi = v; }
    if (hi < lo) continue;
    g.fillRect(x, (1 - hi) * hh / 2, 1, Math.max(1, (hi - lo) * hh / 2));
  }
}
