// Mixer: buses with free routing (sends to any bus, optional master link), inputs, FX slots, presets and meters.
import { h, sel, btn, knob, knobHooks, bus, menu, menuAt, ask, toast, meterPos, clamp } from '../util.js';
import { S, commit, prefs, newFx, addBus, busLabel, reaches, busPresets, saveBusPreset, loadBusPreset, selectedChannels } from '../state.js';
import { actionFor } from '../keys.js';
import { engine } from '../audio/engine.js';
import { FX } from '../audio/fx.js';
import { pluginBody } from './plugui.js';

const dB = v => (v <= 0.001 ? '-∞' : (20 * Math.log10(v)).toFixed(1)) + ' dB';
const pk = v => (v <= 0.001 ? '-∞' : (v >= 1 ? '+' : '') + (20 * Math.log10(v)).toFixed(1));
const LETTERS = 'ABCDEFGHIJKL';

export default function Mixer() {
  const strips = h('div', { class: 'strips' }), rack = h('div', { class: 'fxrack' }), el = h('div', { class: 'view mixer', tabIndex: 0 }, strips, rack);
  const scopes = [];   // [bus id, canvas]: a live waveform on each strip
  const sync = () => engine.graph.sync();
  let cur = 'master', meters = [], arms = [], ctl = new Map(), running = false, last = 0;
  const newInsert = () => { cur = addBus('Insert ' + (S.p.buses.filter(z => z.kind === 'insert').length + 1)).id; commit(); };
  const newSend = () => addBus('Send ' + (LETTERS[S.p.buses.filter(z => z.kind === 'send').length] || '+'), 'send');

  // Rename / reorder / presets / delete, from a right-click on the strip or a click on its name.
  function stripMenu(b, x, y) {
    const P = S.p, same = P.buses.filter(z => z.kind === b.kind), k = same.indexOf(b), fixed = b.kind === 'master', pres = Object.keys(busPresets());
    const swap = d => { const o = same[k + d]; if (!o) return; const i = P.buses.indexOf(b), j = P.buses.indexOf(o); P.buses[i] = o; P.buses[j] = b; commit(); };
    menu(x, y, [
      { head: `${busLabel(P, b.id)} · ${b.name}` },
      { label: 'Rename…', fn: async () => { const v = await ask('Bus name', b.name); if (v) { b.name = v; commit(); } } },
      { label: 'Move left', disabled: fixed || k < 1, fn: () => swap(-1) },
      { label: 'Move right', disabled: fixed || k >= same.length - 1, fn: () => swap(1) },
      { label: 'Reset level & pan', fn: () => { Object.assign(b, { vol: b.kind === 'master' ? 0.9 : 1, pan: 0, width: 1, mono: false, inv: false }); commit(); } },
      '-',
      { label: 'Save as preset…', fn: async () => { const v = await ask('Preset name', b.name); if (v) { saveBusPreset(v, b); toast(`Saved mixer preset “${v}”`); } } },
      ...pres.map(n => ({ label: 'Load preset: ' + n, fn: () => { loadBusPreset(n, b); commit(); toast(`Loaded “${n}” onto ${b.name}`); } })),
      pres.length && { label: 'Delete a preset…', fn: () => menu(x, y, [{ head: 'Delete preset' }, ...pres.map(n => ({ label: n, fn: () => { saveBusPreset(n, null); toast('Preset deleted'); } }))]) },
      '-',
      { label: 'Add insert bus', fn: newInsert },
      { label: 'Add send bus', fn: () => { cur = newSend().id; commit(); } },
      { label: 'Delete bus', disabled: fixed, fn: () => {
        engine.disarm(b);
        P.channels.forEach(c => { if (c.bus === b.id) c.bus = 'master'; }); P.song.audio.forEach(a => { if (a.bus === b.id) a.bus = 'master'; });
        P.buses.forEach(z => { z.sends = z.sends.filter(s => s.to !== b.id); z.fx.forEach(f => { if (f.p.src === b.id) f.p.src = ''; }); });
        P.song.auto = P.song.auto.filter(l => !l.target.startsWith('bus:' + b.id + ':') && !l.target.startsWith('fx:' + b.id + ':'));
        P.buses = P.buses.filter(z => z !== b); cur = 'master'; commit(); toast(`Deleted “${b.name}” - its channels now go to the master`);
      } },
    ]);
  }
  // Pick the microphone / line input that feeds a bus, and whether you hear it while recording.
  async function inputMenu(b, anchor) {
    let devs = [];
    try { devs = (await navigator.mediaDevices.enumerateDevices()).filter(d => d.kind === 'audioinput' && d.deviceId !== 'default' && d.deviceId !== 'communications'); } catch { /* no device access */ }
    const pick = async id => { b.input = id; if (id) await engine.arm(b); else engine.disarm(b); commit(); };
    menuAt(anchor, [
      { head: 'Audio input · ' + b.name },
      { label: '(none)', on: !b.input, fn: () => pick('') },
      { label: 'Default input', on: b.input === 'default', fn: () => pick('default') },
      ...devs.map((d, i) => ({ label: d.label || 'Input ' + (i + 1), on: b.input === d.deviceId, fn: () => pick(d.deviceId) })),
      '-',
      { label: 'Stereo (In 1 + In 2)', on: b.stereo, fn: async () => { b.stereo = !b.stereo; if (b.arm) await engine.arm(b); commit(); } },
      { label: 'Monitor through this bus', on: b.mon, fn: () => { b.mon = !b.mon; engine.wireInputs(); commit(); } },
      !devs.some(d => d.label) && { head: 'Device names appear after the first recording permission' },
    ].filter(Boolean));
  }
  // Where this bus goes: a send (with its own level) to any other bus that would not create a feedback loop.
  function sendMenu(b, anchor) {
    const P = S.p, ok = P.buses.filter(t => t.kind !== 'master' && t.id !== b.id && !b.sends.some(s => s.to === t.id) && !reaches(P, t.id, b.id));
    menuAt(anchor, [{ head: 'Send ' + b.name + ' to' }, ...ok.map(t => ({ label: busLabel(P, t.id) + ' · ' + t.name, fn: () => { b.sends.push({ to: t.id, amt: 0.5 }); commit(); } })),
      !ok.length && { head: 'No bus available (a send may not loop back)' }, '-',
      { label: 'New send bus', fn: () => { const t = newSend(); b.sends.push({ to: t.id, amt: 0.5 }); commit(); } }]);
  }

  function strip(b) {
    const P = S.p, fill = h('i'), db = h('small', { class: 'db' }, dB(b.vol)), st = { v: 0, hold: 0, shown: '' };
    const peak = h('small', { class: 'peak', title: 'Peak level in dB - click to reset', onclick: () => { st.hold = 0; } }, '-∞');
    meters.push([b.id, fill, st, peak]);
    const scope = h('canvas', { class: 'scope', width: 120, height: 34, title: 'Waveform of what this bus plays' }); scopes.push([b.id, scope]);
    const tog = (key, label, title, cls = '') => {
      const x = h('button', { class: 'mini ' + cls + (b[key] ? ' on' : ''), title, onclick: () => { b[key] = !b[key]; x.classList.toggle('on', b[key]); sync(); commit(''); } }, label);
      return x;
    };
    const auto = key => v => engine.autoWrite(`bus:${b.id}:${key}`, v);
    const k = (key, label, o = {}) => knob({ label, size: 'xs', value: b[key], onInput: v => { b[key] = v; sync(); o.auto && o.auto(v); }, onChange: () => commit(''), ...o });
    const armB = b.kind !== 'master' && h('button', { class: 'mini arm' + (b.arm ? ' on' : ''), title: 'Arm for recording: with Rec on, playing in SONG mode records this input onto the song timeline',
      onclick: async () => { if (b.arm) engine.disarm(b); else { b.input ||= 'default'; await engine.arm(b); } commit(); } }, '●');
    if (armB && b.arm) arms.push([b.id, armB]);
    const panK = k('pan', 'Pan', { min: -1, max: 1, bi: true, def: 0, auto: auto('pan'), link: `bus:${b.id}:pan` });
    const fader = h('input', { class: 'fader', type: 'range', min: 0, max: 1.25, step: 0.005, value: b.vol,
      oninput: e => { b.vol = +e.target.value; db.textContent = dB(b.vol); sync(); auto('vol')(b.vol); }, onchange: () => commit(''),
      ondblclick: e => { e.target.value = b.vol = 1; db.textContent = dB(1); sync(); commit(''); } });
    // The fader gets the same right-click menu as knobs: reset, type a level, automation, MIDI controller link.
    fader.dataset.link = `bus:${b.id}:vol`; fader.set = v => { fader.value = v; db.textContent = dB(v); };
    fader.addEventListener('contextmenu', e => {
      e.preventDefault(); e.stopPropagation();
      const setV = v => { b.vol = clamp(v, 0, 1.25); fader.set(b.vol); sync(); commit(''); };
      menu(e.clientX, e.clientY, [{ head: b.name + ' level · ' + dB(b.vol) }, { label: 'Reset (0 dB)', fn: () => setV(1) },
        { label: 'Type in level (dB)…', fn: async () => { const t = await ask('Level in dB (up to +1.9)', (20 * Math.log10(Math.max(1e-4, b.vol))).toFixed(1)); if (t != null && isFinite(+t)) setV(10 ** (+t / 20)); } },
        ...knobHooks.items(`bus:${b.id}:vol`, b.vol)]);
    });
    ctl.set(b.id, { fader, db, panK });
    return h('div', { class: 'strip ' + b.kind + (b.id === cur ? ' sel' : ''), 'data-id': b.id, oncontextmenu: e => { e.preventDefault(); stripMenu(b, e.clientX, e.clientY); },
      onpointerdown: () => { if (cur !== b.id) { cur = b.id; [...strips.children].forEach(s => s.classList.toggle('sel', s.dataset.id === cur)); fxPanel(); } } },
      h('button', { class: 'sname', title: 'Bus options: rename, move, presets, delete (or right-click the strip)', onclick: e => { const r = e.currentTarget.getBoundingClientRect(); stripMenu(b, r.left, r.bottom); } }, h('i', null, busLabel(P, b.id)), b.name),
      scope,
      h('div', { class: 'fxleds' }, b.fx.map(f => h('button', { class: 'fxled' + (f.on ? ' on' : ''), title: FX[f.type].name + ' (click to bypass)', onclick: () => toggleFx(b, f) }, FX[f.type].name.slice(0, 4)))),
      h('div', { class: 'krow' }, panK, k('width', 'Width', { max: 2, def: 1, fmt: v => Math.round(v * 100) + '%' })),
      h('div', { class: 'brow' }, tog('mute', 'M', 'Mute', 'm'), b.kind === 'insert' && tog('solo', 'S', 'Solo', 's'), tog('inv', 'Ø', 'Reverse polarity'), tog('mono', '1', 'Mono (sum left + right)')),
      b.kind !== 'master' && h('div', { class: 'brow' }, h('button', { class: 'mini inb' + (b.input ? ' on' : ''), title: 'Audio input (microphone / line in) for this bus', onclick: e => inputMenu(b, e.currentTarget) }, 'IN ▾'), armB,
        h('button', { class: 'mini' + (b.mon ? ' on' : ''), title: 'Monitor: hear the input through this bus', onclick: () => { b.mon = !b.mon; engine.wireInputs(); commit(); } }, 'Mon')),
      h('div', { class: 'frow' }, fader, h('div', { class: 'meter' }, fill)),
      h('div', { class: 'dbrow' }, db, peak),
      b.kind !== 'master' && h('div', { class: 'routes' },
        tog('toMaster', '→ Master', 'Link to the master bus. Switch off to hear this bus only through its sends.', 'tom'),
        b.sends.map(s => h('div', { class: 'send', title: 'Send to ' + (P.buses.find(z => z.id === s.to) || {}).name + ' - right-click to remove', oncontextmenu: e => { e.preventDefault(); e.stopPropagation(); b.sends = b.sends.filter(z => z !== s); commit(); } },
          knob({ label: '→ ' + busLabel(P, s.to), size: 'xs', value: s.amt, def: 0.5, max: 1.25, fmt: v => Math.round(v * 100) + '%', onInput: v => { s.amt = v; sync(); }, onChange: () => commit('') }))),
        h('button', { class: 'mini addsend', title: 'Add a send to another bus', onclick: e => sendMenu(b, e.currentTarget) }, '+ send')));
  }
  function toggleFx(b, f) { f.on = !f.on; sync(); commit(''); build(); }
  function fxPanel() {
    const P = S.p, b = P.buses.find(x => x.id === cur) || P.buses[0];
    rack.replaceChildren(
      h('div', { class: 'fxhead' }, h('b', null, 'FX slots · ' + b.name),
        h('button', { class: 'btn primary sm', onclick: e => menuAt(e.currentTarget, Object.entries(FX).map(([type, d]) => ({ label: d.name, fn: () => { b.fx.push(newFx(type)); commit(); } }))) }, '+ Add effect')),
      ...(b.fx.length ? b.fx.map((f, i) => {
        const d = FX[f.type], node = () => { const n = engine.graph.buses.get(b.id); return n && n.fx[i]; };
        const move = dir => { const j = i + dir; if (j < 0 || j >= b.fx.length) return; b.fx.splice(i, 1); b.fx.splice(j, 0, f); commit(); };
        return h('div', { class: 'fxcard' + (f.on ? '' : ' off') },
          h('div', { class: 'fxtitle' }, h('button', { class: 'led' + (f.on ? ' on' : ''), title: 'Enable / bypass', onclick: () => toggleFx(b, f) }), h('b', null, d.name), h('span', { class: 'grow' }),
            btn('↑', () => move(-1), 'sm', 'Move earlier in the chain'), btn('↓', () => move(1), 'sm', 'Move later'), btn('✕', () => { b.fx.splice(i, 1); commit(); }, 'sm', 'Remove')),
          f.type === 'plugin' ? pluginBody(f.p, node(), { want: 'fx', autoKey: 'fx:' + b.id + ':' + f.id + ':', onPick: () => { sync(); commit(''); fxPanel(); }, onChange: () => commit('') }) : h('div', { class: 'krow wrap' }, d.params.map(q => q.buses
            ? h('label', { class: 'pick' }, q.label, sel([['', '(choose a bus)'], ...P.buses.filter(z => z.id !== b.id && z.kind !== 'master').map(z => [z.id, busLabel(P, z.id) + ' · ' + z.name])], f.p[q.k] || '', v => { f.p[q.k] = v; sync(); commit(''); }))
            : q.opts
              ? h('label', { class: 'pick' }, q.label, sel(q.opts, f.p[q.k], v => { f.p[q.k] = v; const n = node(); n && n.set(q.k, v); commit(''); }))
              : knob({ label: q.label, size: 'sm', min: q.min, max: q.max, curve: q.curve, step: q.step, bi: q.bi, fmt: q.fmt, def: q.def, value: f.p[q.k], link: 'fx:' + b.id + ':' + f.id + ':' + q.k, onInput: v => { f.p[q.k] = v; const n = node(); n && n.set(q.k, v); engine.autoWrite('fx:' + b.id + ':' + f.id + ':' + q.k, v); }, onChange: () => commit('') }))),
          f.type === 'duck' && h('small', null, 'Put this on the sound that should move out of the way (bass, pad) and choose the bus that triggers it (kick). “One frequency band” ducks only around Band Hz, like linking the peak to an EQ band.'));
      }) : [h('small', { class: 'empty' }, 'No effects on this bus. Add EQ, compression, reverb, delay, a peak controller for ducking and more, then toggle each slot on or off.')]));
  }
  function build() {
    meters = []; arms = []; ctl = new Map(); scopes.length = 0;
    if (!S.p.buses.some(b => b.id === cur)) cur = 'master';
    strips.replaceChildren(...S.p.buses.map(strip), h('button', { class: 'addbus', title: 'Click: add an insert bus · Right-click: add a send bus', onclick: newInsert, oncontextmenu: e => { e.preventDefault(); cur = newSend().id; commit(); } }, '+'));
    fxPanel();
  }
  function tick(ts) {
    if (!el.isConnected) { running = false; return; }
    requestAnimationFrame(tick);
    if (!prefs.meterFps || ts - last < 1000 / prefs.meterFps - 2) return;
    last = ts;
    for (const [id, fill, st, peak] of meters) {
      const lv = engine.level(id); st.v = Math.max(lv, st.v * 0.82); st.hold = Math.max(lv, st.hold * 0.992);
      fill.style.setProperty('--lv', meterPos(st.v).toFixed(3)); fill.classList.toggle('clip', lv > 1);
      const t = pk(st.hold); if (t !== st.shown) { st.shown = t; peak.textContent = t; peak.classList.toggle('hot', st.hold >= 1); }
    }
    for (const [id, b] of arms) b.style.setProperty('--lv', Math.min(1, Math.sqrt(engine.inLevel(id))).toFixed(2));
    const ink = getComputedStyle(el).getPropertyValue('--accent').trim() || '#6ea8ff';
    for (const [id, cv] of scopes) {
      const d = engine.wave(id), g = cv.getContext('2d'), W = cv.width, H = cv.height; g.clearRect(0, 0, W, H);
      g.fillStyle = 'rgba(255,255,255,.06)'; g.fillRect(0, H / 2, W, 1); if (!d) continue;
      // A scrolling waveform: one column per frame holding that frame's lowest and highest sample, newest on the right.
      // A single 10 ms snapshot is mostly silence between hits, so the history shows the bus far better.
      let lo = 0, hi = 0; for (let i = 0; i < d.length; i++) { const v = d[i]; if (v < lo) lo = v; else if (v > hi) hi = v; }
      const cols = cv.cols || (cv.cols = new Float32Array(W * 2)); cols.copyWithin(0, 2); cols[W * 2 - 2] = lo; cols[W * 2 - 1] = hi;
      // scaled to the loudest column on screen (but never more than x8), so quiet buses still show their shape
      let pk = 0; for (let i = 0; i < cols.length; i++) { const a = Math.abs(cols[i]); if (a > pk) pk = a; }
      const k = (H / 2 - 1) / Math.max(0.125, pk); g.fillStyle = ink;
      for (let x = 0; x < W; x++) { const a = H / 2 - cols[x * 2 + 1] * k, b = H / 2 - cols[x * 2] * k; g.fillRect(x, a, 1, Math.max(1, b - a)); }
    }
  }
  // Faders and pans follow automation as it plays.
  let fxDraw = 0;
  bus.on('auto', () => { if (!el.isConnected) return; if (S.p.song.auto.some(l => l.target.startsWith('fx:' + cur + ':')) && performance.now() - fxDraw > 250) { fxDraw = performance.now(); fxPanel(); } for (const b of S.p.buses) { const c = ctl.get(b.id); if (c) { c.fader.value = b.vol; c.db.textContent = dB(b.vol); c.panK.set(b.pan); } } });
  bus.on('project', build);
  let plugDraw = 0;
  bus.on('plugin', () => { if (el.isConnected && !plugDraw) plugDraw = requestAnimationFrame(() => { plugDraw = 0; fxPanel(); }); });
  build();
  el.addEventListener('keydown', e => {
    const id = actionFor(e, 'mixer'), P = S.p, b = P.buses.find(z => z.id === cur); if (!id || !b) return;
    const same = P.buses.filter(z => z.kind === b.kind), k = same.indexOf(b);
    const swap = d => { const o = same[k + d]; if (!o || b.kind === 'master') return; const i = P.buses.indexOf(b), j = P.buses.indexOf(o); P.buses[i] = o; P.buses[j] = b; commit(); };
    const acts = {
      'mix.rename': async () => { const v = await ask('Bus name', b.name); if (v) { b.name = v; commit(); } },
      'mix.solo': () => { if (b.kind !== 'insert') return toast('Only insert buses can be soloed'); b.solo = !b.solo; sync(); commit(); },
      'mix.left': () => swap(-1), 'mix.right': () => swap(1),
      'mix.link': () => { const ids = selectedChannels(); for (const c of P.channels) if (ids.has(c.id)) c.bus = b.id; commit(); toast(`${ids.size} channel${ids.size === 1 ? '' : 's'} now play through ${b.name}`); },
      'mix.render': () => bus.emit('renderArmed'),
    };
    if (!acts[id]) return; e.preventDefault(); e.stopPropagation(); acts[id]();
  });
  return { el, show() { if (!running) { running = true; requestAnimationFrame(tick); } } };
}
