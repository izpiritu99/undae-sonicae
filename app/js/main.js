// App shell: boot, transport bar, two-deck workspace, on-screen keyboard, shortcuts, project file I/O.
import { h, bus, sel, btn, icon, knob, knobHooks, menuAt, closeMenu, ask, toast, modal, pickFile, download, clamp, meterPos, uid, isBlack, BAR, BEAT, SIG, PPQ, STEP } from './util.js';
import { S, prefs, savePrefs, setProject, demoProject, blankProject, loadSaved, stashLast, lastSession, templates, saveTemplate, fromTemplate, commit, undo, histPos, newChannel, reaches, pat, chan, newPattern, songBars, fitBars, changeSig } from './state.js';
import { engine, collect } from './audio/engine.js';
import { buildKit } from './audio/drums.js';
import { initSamples, records, addBytes, samples, wavBytes, loadFolders } from './samples.js';
import { midi, writeMidi, readMidi } from './midi.js';
import Settings, { applyPrefs } from './ui/settings.js';
import Rack from './ui/rack.js';
import Roll from './ui/pianoroll.js';
import Song from './ui/song.js';
import Mixer from './ui/mixer.js';
import Inst from './ui/inst.js';
import Lab from './ui/lab.js';
import Library from './ui/lib.js';
import Design from './ui/design.js';
import { actionFor, ACTIONS, SCOPES, keyOf } from './keys.js';
import { learn, linksFor, unlink, cancelLearn } from './links.js';
import { encodeOpus, encodeMp3 } from './audio/encode.js';
import { choosePlugin, slotFor } from './ui/plugui.js';
import './touch.js';   // long-press = right-click, touch-only controls
import { setPlugServer } from './audio/bridge.js';
setPlugServer(prefs.plugServer);   // a plug-in server on another computer (Settings › Plug-ins), before any plug-in loads

const VIEWS = [['rack', 'Rack', Rack], ['roll', 'Piano Roll', Roll], ['song', 'Song', Song], ['mixer', 'Mixer', Mixer], ['inst', 'Instrument', Inst], ['design', 'Sound Design', Design], ['lab', 'Audio Lab', Lab], ['lib', 'Library', Library], ['set', 'Settings', Settings]];
const views = {}, app = document.getElementById('app'), T = {};   // T: transport actions the shortcut table can call
const narrow = matchMedia('(max-width: 760px), (pointer: coarse) and (max-height: 560px)');   // phones, upright or sideways: one deck


// --- project files ------------------------------------------------------
const b64 = buf => { const u = new Uint8Array(buf); let s = ''; for (let i = 0; i < u.length; i += 0x8000) s += String.fromCharCode.apply(null, u.subarray(i, i + 0x8000)); return btoa(s); };
const unb64 = s => Uint8Array.from(atob(s), c => c.charCodeAt(0)).buffer;
const safe = s => s.replace(/[^\w\- ]+/g, '').trim() || 'project';
async function saveFile() {
  const used = new Set([...S.p.channels.map(c => c.sample), ...S.p.song.audio.map(a => a.sample)]), recs = (await records()).filter(r => used.has(r.id));
  download(new Blob([JSON.stringify({ app: 'undae-sonicae', project: S.p, samples: recs.map(r => ({ id: r.id, name: r.name, data: b64(r.bytes) })) })], { type: 'application/json' }), safe(S.p.name) + '.undae.json');
}
async function openFile() {
  const [f] = await pickFile('.json,application/json'); if (!f) return;
  try {
    const d = JSON.parse(await f.text()); if (!d.project || !d.project.channels) throw 0;
    for (const s of d.samples || []) if (!samples.has(s.id)) await addBytes(s.name, unb64(s.data), s.id);
    setProject(d.project); toast('Opened ' + d.project.name);
  } catch { toast('That is not an Undae Sonicae project file'); }
}
// Compressed audio: MP3 (LAME, in a worker) or Ogg Opus (the browser's own encoder), both faster than real time (audio/encode.js).
async function exportComp(mode, fmt = prefs.compFormat || 'mp3') {
  toast('Rendering audio…', 60000);
  try {
    const buf = await engine.render(mode, mode === 'song' ? 1 : 4), name = `${safe(S.p.name)}-${mode === 'song' ? 'song' : safe(pat().name)}`;
    if (fmt === 'opus') { download(await encodeOpus(buf, (prefs.opusKbps || 192) * 1000), name + '.opus'); return toast(`Exported Ogg Opus (${prefs.opusKbps || 192} kbps)`); }
    const kb = prefs.mp3Kbps || 320; toast('Encoding MP3…', 60000);
    download(await encodeMp3(buf, kb, f => toast(`Encoding MP3… ${Math.round(f * 100)}%`, 60000)), name + '.mp3'); toast(`Exported MP3 (${kb} kbps)`);
  } catch (e) { toast(e.message); }
}
// Alt+R in the Mixer: one WAV per armed bus, with only the channels and audio clips that reach that bus playing.
bus.on('renderArmed', async () => {
  const P = S.p, armed = P.buses.filter(b => b.arm && b.kind !== 'master');
  if (!armed.length) return toast('Arm one or more buses first (the red dot on a strip)');
  const mode = S.mode === 'song' ? 'song' : 'pat', keepC = P.channels.map(c => c.mute), keepA = P.song.audio.map(a => a.mute);
  try {
    for (const b of armed) {
      toast(`Rendering ${b.name}…`, 60000);
      P.channels.forEach((c, i) => { c.mute = keepC[i] || !reaches(P, c.bus || 'master', b.id); });
      P.song.audio.forEach((a, i) => { a.mute = keepA[i] || !reaches(P, a.bus || 'master', b.id); });
      const buf = await engine.render(mode, mode === 'song' ? 1 : 4);
      download(new Blob([wavBytes(buf)], { type: 'audio/wav' }), `${safe(P.name)}-${safe(b.name)}.wav`);
    }
    toast(`Rendered ${armed.length} bus${armed.length === 1 ? '' : 'es'}`);
  } catch (e) { toast(e.message); }
  finally { P.channels.forEach((c, i) => { c.mute = keepC[i]; }); P.song.audio.forEach((a, i) => { a.mute = keepA[i]; }); engine.refresh(); }
});
async function exportWav(mode) {
  toast('Rendering audio…', 60000);
  try { const buf = await engine.render(mode, mode === 'song' ? 1 : 4); download(new Blob([wavBytes(buf)], { type: 'audio/wav' }), `${safe(S.p.name)}-${mode === 'song' ? 'song' : safe(pat().name)}.wav`); toast(buf.note ? 'WAV exported, but ' + buf.note : 'WAV exported', buf.note ? 9000 : undefined); }
  catch (e) { toast(e.message); }
}
function exportMidi() {
  const mode = S.mode, L = mode === 'song' ? songBars(S.p) * BAR : pat().bars * BAR;
  if (!L) return toast('Nothing to export yet');
  const by = new Map();
  for (const e of collect(S.p, mode, pat(), 0, L)) { if (!by.has(e.c)) by.set(e.c, []); by.get(e.c).push({ t: e.tick + e.sw, d: e.d, n: e.n, v: e.v }); }
  download(new Blob([writeMidi([...by].map(([c, notes]) => ({ name: c.name, notes })), S.p.bpm)], { type: 'audio/midi' }), safe(S.p.name) + '.mid');
}
async function importMidi() {
  const [f] = await pickFile('.mid,.midi,audio/midi'); if (!f) return;
  try {
    const c = chan(); if (!c) return toast('Add a channel in the Rack first');
    const { notes } = readMidi(await f.arrayBuffer()), p = pat(), ns = notes.filter(n => n.t < 64 * BAR);
    if (!ns.length) return toast('No notes in that file');
    p.notes[c.id] = ns.map(n => ({ id: uid(), t: n.t, d: n.d, n: n.n, v: n.v }));
    p.bars = Math.max(p.bars, fitBars(ns.reduce((m, n) => Math.max(m, n.t + n.d), 0)));
    commit(); bus.emit('goto', { to: 'roll' }); toast(`Imported ${ns.length} notes into “${c.name}”`);
  } catch (e) { toast(e.message || 'Could not read that MIDI file'); }
}

// --- top bar --------------------------------------------------------------
function topbar() {
  const playB = h('button', { class: 'tb big', title: 'Play / stop (Space)', onclick: () => engine.toggle() }, icon('play'));
  // Record asks what to capture: played notes, armed microphone inputs, fader and knob moves - any mix of the three.
  const recB = h('button', { class: 'tb rec', title: 'Record - choose what to capture, then press play', onclick: () => {
    if (S.rec) { S.rec = false; recB.classList.remove('on'); return; }
    const w = prefs.recWhat, box = (obj, key, label, hint) => h('label', { class: 'recopt' }, h('input', { type: 'checkbox', checked: !!obj[key], onchange: e => { obj[key] = e.target.checked; } }), h('div', null, h('b', null, label), h('small', null, hint)));
    modal('Record', h('div', { class: 'form' },
      box(w, 'midi', 'Notes (MIDI)', 'What you play on a MIDI keyboard, the on-screen keys or the typing keys, into the selected channel. PAT mode.'),
      box(w, 'audio', 'Microphone / line audio', 'Every mixer bus with an input chosen and its red dot armed, onto the song timeline. SONG mode.'),
      box(w, 'auto', 'Automation', 'Fader and pan moves in the mixer and rack while the song plays. SONG mode.'),
      h('hr'), box(prefs, 'loopRec', 'Loop record', 'Keep cycling and layer each pass (audio passes become stacked takes). Off: record one pass, then stop.')),
      [{ label: 'Cancel' }, { label: 'Arm recording', primary: true, fn: () => { if (!w.midi && !w.audio && !w.auto) { toast('Tick at least one thing to record'); return false; } savePrefs(); S.rec = true; recB.classList.add('on'); toast('Armed - press play to start recording'); } }]);
  } }, icon('rec'));
  const metB = h('button', { class: 'tb', title: 'Metronome', onclick: () => { S.metro = !S.metro; metB.classList.toggle('on', S.metro); } }, icon('metro'));
  const modeB = h('div', { class: 'seg', title: 'Play the current pattern or the whole song' }, ['pat', 'song'].map(m => h('button', { 'data-m': m, onclick: () => { if (S.mode === m) return; const was = engine.playing; if (was) engine.stop(); S.mode = m; drawMode(); if (was) engine.play(); } }, m === 'pat' ? 'PAT' : 'SONG')));
  const drawMode = () => [...modeB.children].forEach(b => b.classList.toggle('on', b.dataset.m === S.mode));
  const bpm = h('input', { class: 'bpm', type: 'number', min: 40, max: 260, step: 1, title: 'Tempo (BPM)', onchange: () => { engine.setBpm(clamp(+bpm.value || 120, 40, 260)); bpm.value = S.p.bpm; commit(''); }, onkeydown: e => e.stopPropagation() });
  const sigB = h('button', { class: 'btn sm sig', title: 'Time signature', onclick: e => menuAt(e.currentTarget, [{ head: 'Time signature' }, ...[[2, 4], [3, 4], [4, 4], [5, 4], [6, 4], [7, 4], [3, 8], [5, 8], [6, 8], [7, 8], [9, 8], [12, 8]].map(([n, d]) => ({ label: n + ' / ' + d, on: SIG[0] === n && SIG[1] === d, fn: () => { const was = engine.playing; if (was) engine.stop(); changeSig(n, d); if (was) engine.play(); } }))]) });
  // Scroll over the tempo to change it: 1 BPM per notch, 5 with Shift.
  let bpmT; bpm.addEventListener('wheel', e => { e.preventDefault(); const v = clamp(Math.round(S.p.bpm) + (e.deltaY < 0 ? 1 : -1) * (e.shiftKey ? 5 : 1), 40, 260); engine.setBpm(v); bpm.value = v; clearTimeout(bpmT); bpmT = setTimeout(() => commit(''), 400); }, { passive: false });
  let taps = [];
  const tapB = btn('Tap', () => { const t = performance.now(); taps = taps.filter(x => t - x < 2500); taps.push(t); if (taps.length > 2) { const v = clamp(Math.round(60000 / ((t - taps[0]) / (taps.length - 1))), 40, 260); engine.setBpm(v); bpm.value = v; commit(''); } }, 'sm', 'Tap tempo');
  const patBox = h('span', { class: 'patbox' }), name = h('span');
  // Song position, switchable between bars:steps:ticks and minutes:seconds:centiseconds.
  const two = n => String(n).padStart(2, '0');
  const fmtPos = t => { if (prefs.timeFmt === 'msc') { const s = t * engine.spt(); return `${Math.floor(s / 60)}:${two(Math.floor(s % 60))}:${two(Math.floor(s * 100) % 100)}`; } return `${Math.floor(t / BAR) + 1}:${two(Math.floor(t % BAR / STEP) + 1)}:${two(Math.floor(t % STEP))}`; };
  const posLab = h('small', null, ''), posVal = h('b', null, '');
  const drawPos = t => { posVal.textContent = fmtPos(t); posLab.textContent = prefs.timeFmt === 'msc' ? 'M:S:CS' : 'B:S:T'; };
  const pos = h('button', { class: 'pos', title: 'Song position - click to switch between bars:steps:ticks and minutes:seconds:centiseconds', onclick: () => { prefs.timeFmt = prefs.timeFmt === 'msc' ? 'bst' : 'msc'; savePrefs(); drawPos(engine.playing ? engine.pos() : 0); } }, posVal, posLab);
  drawPos(0);
  const mFill = h('i'), mDb = h('small', null, '-∞ dB'), mMeter = h('div', { class: 'mmeter', title: 'Master output level (peak, dB)' }, h('div', null, mFill), mDb);
  let mHold = 0, mShown = '';
  setInterval(() => {
    if (!engine.graph || document.hidden) return;
    const lv = engine.level('master'); mHold = Math.max(lv, mHold * 0.97);
    mFill.style.setProperty('--lv', meterPos(lv).toFixed(3)); mFill.classList.toggle('clip', lv >= 1);
    const t = (mHold <= 0.001 ? '-∞' : (mHold >= 1 ? '+' : '') + (20 * Math.log10(mHold)).toFixed(1)) + ' dB'; if (t !== mShown) { mShown = t; mDb.textContent = t; mDb.classList.toggle('hot', mHold >= 1); }
  }, 60);
  // Master pitch: transposes every instrument (built-in in cents, plug-ins in whole semitones). Right-click for more.
  const mPitch = knob({ label: 'Pitch', size: 'xs', min: -12, max: 12, step: 0.01, bi: true, def: 0, value: S.p.masterPitch || 0, link: 'master:pitch', fmt: v => (v > 0 ? '+' : '') + v.toFixed(Math.abs(v - Math.round(v)) < 0.005 ? 0 : 2) + ' st', onInput: v => { S.p.masterPitch = Math.abs(v) < 0.02 ? 0 : v; }, onChange: () => commit('') });
  bus.on('project', () => mPitch.set(S.p.masterPitch || 0));
  // Load: CPU and memory of the app and its plug-in hosts (desktop app, /stats.json), playing voices, CPU history.
  const cpuV = h('b', null, '--'), ramV = h('b', null, '--'), voV = h('b', null, '0'), hist = h('canvas', { class: 'cpuhist', width: 60, height: 22 }), H = [];
  const stats = h('div', { class: 'grp stats', title: 'CPU: share of all processor cores used by Undae Sonicae and its plug-in hosts · RAM: memory in use · voices: sounds playing now' },
    h('div', null, h('small', null, 'CPU'), cpuV), hist, h('div', null, h('small', null, 'RAM'), ramV), h('div', null, h('small', null, 'VOICES'), voV));
  let statsOk = true;
  setInterval(async () => {
    if (document.hidden) return;
    voV.textContent = String(engine.voiceCount ? engine.voiceCount() : 0);
    if (!statsOk) return;
    try {
      const r = await fetch('stats.json', { cache: 'no-store' }); if (!r.ok) throw 0; const j = await r.json(); if (!j) throw 0;
      cpuV.textContent = Math.round(j.cpu) + '%'; ramV.textContent = j.ram >= 1024 ? (j.ram / 1024).toFixed(1) + ' GB' : Math.round(j.ram) + ' MB';
      H.push(j.cpu); if (H.length > 60) H.shift();
      const g = hist.getContext('2d'); g.clearRect(0, 0, 60, 22);
      H.forEach((v, i) => { g.fillStyle = v > 80 ? '#e5484d' : v > 50 ? '#f5c542' : '#3ecf8e'; const y = Math.max(1, Math.min(22, v / 100 * 22)); g.fillRect(i + 60 - H.length, 22 - y, 1, y); });
    } catch { statsOk = false; cpuV.textContent = '—'; ramV.textContent = '—'; stats.title = 'CPU and memory are shown in the desktop app'; }
  }, 1000);
  const midiB = h('button', { class: 'tb', title: 'MIDI devices', onclick: async e => { const t = e.currentTarget; if (!midi.ok) await midi.init(); menuAt(t, midi.ok ? [{ head: midi.inputs.length ? 'Connected inputs' : 'MIDI on - no devices found' }, ...midi.inputs.map(i => ({ label: i.name || 'MIDI input', on: true })), '-', { label: 'Rescan', fn: () => midi.init() }] : [{ head: midi.err }]); } }, icon('midi'));
  const keysB = h('button', { class: 'tb' + (prefs.keys ? ' on' : ''), title: 'On-screen keyboard', onclick: () => { prefs.keys = !prefs.keys; savePrefs(); keysB.classList.toggle('on', prefs.keys); bus.emit('layout'); } }, icon('keys'));
  const patMenu = e => menuAt(e.currentTarget, [
    { label: 'New pattern', fn: async () => { const v = await ask('Pattern name', 'Pattern ' + (S.p.patterns.length + 1)); if (v) { const p = newPattern(v); S.p.patterns.push(p); S.pat = p.id; commit(); bus.emit('sel'); } } },
    { label: 'Duplicate pattern', fn: () => { const p = JSON.parse(JSON.stringify(pat())); p.id = uid(); p.name += ' copy'; for (const k in p.notes) p.notes[k].forEach(n => { n.id = uid(); }); S.p.patterns.push(p); S.pat = p.id; commit(); bus.emit('sel'); } },
    { label: 'Rename…', fn: async () => { const v = await ask('Pattern name', pat().name); if (v) { pat().name = v; commit(); } } },
    { label: 'Split by channel', fn: () => {
      const src = pat(), made = S.p.channels.filter(c => (src.notes[c.id] || []).length).map(c => { const p = newPattern(`${src.name} · ${c.name}`, src.bars); p.notes[c.id] = src.notes[c.id].map(n => ({ ...n, id: uid() })); return p; });
      if (made.length < 2) return toast('This pattern only uses one channel');
      S.p.patterns.push(...made); commit(); toast(`Made ${made.length} patterns, one per channel (the original is kept)`);
    } },
    { label: 'Clear notes', fn: () => { pat().notes = {}; commit(); } },
    { label: 'Delete pattern', disabled: S.p.patterns.length < 2, fn: () => { const id = pat().id; S.p.patterns = S.p.patterns.filter(p => p.id !== id); S.p.song.clips = S.p.song.clips.filter(c => c.pat !== id); S.pat = S.p.patterns[0].id; commit(); bus.emit('sel'); } },
  ]);
  const fileMenu = e => menuAt(e.currentTarget, [
    { head: S.p.name },
    { label: 'Rename project…', fn: async () => { const v = await ask('Project name', S.p.name); if (v) { S.p.name = v; commit(); } } },
    { label: 'New project', fn: () => { stashLast(S.p); setProject(blankProject()); } }, { label: 'Reopen last session', disabled: !lastSession(), fn: () => { const p = lastSession(); if (p) { setProject(p); toast('Reopened ' + p.name); } } }, { label: 'Open project…', fn: openFile }, ...Object.keys(templates()).map(n => ({ label: 'New from template: ' + n, fn: () => { stashLast(S.p); setProject(fromTemplate(n)); toast('New project from “' + n + '”'); } })),
    { label: 'Save as template…', fn: async () => { const v = await ask('Template name (a new project can start from it, also at start-up)', S.p.name === 'Untitled' ? 'My template' : S.p.name); if (v) toast(saveTemplate(v, S.p) ? `Template “${v}” saved` : 'Not enough storage for this template'); } }, { label: 'Save project file', hint: 'Ctrl+S', fn: saveFile },
    '-',
    { label: 'Export WAV · pattern ×4', fn: () => exportWav('pat') }, { label: 'Export WAV · song', hint: 'Ctrl+R', fn: () => exportWav('song') },
    { label: 'Export MP3 · song', hint: prefs.compFormat !== 'opus' ? 'Ctrl+Shift+R' : '', fn: () => exportComp('song', 'mp3') }, { label: 'Export MP3 · pattern ×4', fn: () => exportComp('pat', 'mp3') }, { label: 'Export Ogg Opus · song', hint: prefs.compFormat === 'opus' ? 'Ctrl+Shift+R' : '', fn: () => exportComp('song', 'opus') },
    { label: `Export MIDI · ${S.mode === 'song' ? 'song' : 'pattern'}`, fn: exportMidi }, { label: 'Import MIDI into channel…', fn: importMidi },
    '-',
    { label: 'Undo', hint: 'Ctrl+Z', fn: () => undo(-1) }, { label: 'Redo', hint: 'Ctrl+Shift+Z', fn: () => undo(1) },
  ]);
  const sync = () => {
    bpm.value = S.p.bpm; name.textContent = S.p.name; drawMode(); sigB.textContent = SIG[0] + '/' + SIG[1];
    patBox.replaceChildren(sel(S.p.patterns.map(p => [p.id, p.name]), pat().id, v => { S.pat = v; bus.emit('sel'); }, { title: 'Current pattern' }), h('button', { class: 'tb', title: 'Pattern options', onclick: patMenu }, '⋯'));
  };
  bus.on('project', sync); bus.on('sel', sync);
  bus.on('transport', () => { playB.classList.toggle('on', engine.playing); playB.replaceChildren(icon(engine.playing ? 'stop' : 'play')); if (engine.playing) requestAnimationFrame(frame); else { bus.emit('pos', -1); drawPos(0); } });
  bus.on('midiact', () => { midiB.classList.add('on'); clearTimeout(midiB._t); midiB._t = setTimeout(() => midiB.classList.remove('on'), 150); });
  function frame() {
    if (!engine.playing) return;
    const t = engine.pos(); drawPos(t);
    if (engine.autoDirty) { engine.autoDirty = false; bus.emit('auto'); }
    bus.emit('pos', t); requestAnimationFrame(frame);
  }
  sync();
  // Quick record toggle (R): arms with the last choice of what to record, without the dialog.
  const recQuick = () => { const w = prefs.recWhat; if (!S.rec && !w.midi && !w.audio && !w.auto) return recB.click(); S.rec = !S.rec; recB.classList.toggle('on', S.rec); toast(S.rec ? 'Recording armed - press play' : 'Recording off'); };
  const saveAs = async () => { const v = await ask('Save as (project name)', S.p.name); if (v) { S.p.name = v; commit(''); sync(); saveFile(); } };
  const saveNew = () => { const m = S.p.name.match(/^(.*?)(?: v(\d+))?$/); S.p.name = `${m[1]} v${(+m[2] || 1) + 1}`; commit(''); sync(); saveFile(); toast('Saved as ' + S.p.name); };
  Object.assign(T, { recQuick, saveAs, saveNew, rec: () => recB.click(), metro: () => metB.click(), mode: () => [...modeB.children].find(b => b.dataset.m !== S.mode).click(), time: () => pos.click(), keys: () => keysB.click(),
    pat: d => { const i = S.p.patterns.indexOf(pat()), n = S.p.patterns[i + d]; if (n) { S.pat = n.id; bus.emit('sel'); } } });
  return h('header', { id: 'top' },
    h('button', { class: 'brand', onclick: fileMenu, title: 'Project menu' }, h('img', { class: 'wordmark', src: 'img/wordmark.png', alt: 'Undae Sonicae' }), name, h('i', null, '▾')),
    h('div', { class: 'grp' }, playB, recB, metB, modeB),
    h('div', { class: 'grp' }, bpm, h('small', null, 'BPM'), tapB, sigB),
    h('div', { class: 'grp' }, patBox, pos, mMeter, mPitch),
    stats,
    h('span', { class: 'grow' }),
    h('div', { class: 'grp' }, h('button', { class: 'tb', title: 'Undo', onclick: () => undo(-1) }, icon('undo')), h('button', { class: 'tb', title: 'Redo', onclick: () => undo(1) }, icon('redo')), midiB, keysB));
}

// --- decks ----------------------------------------------------------------
// Up to four docked decks (A-D) plus an optional floating one (F). Each view lives in at most one deck.
const DECKS = ['A', 'B', 'C', 'D', 'F'].map(id => ({ id, cur: null })), [A, B, , , F] = DECKS;
const COUNT = { single: 1, stack: 2, side: 2, tri: 3, quad: 4 };
const active = () => { if (narrow.matches) return [A]; const list = DECKS.slice(0, COUNT[prefs.layout] || 2); if (prefs.float) list.push(F); return list; };
const free = () => VIEWS.map(v => v[0]).find(id => !active().some(d => d.cur === id));
let decksEl;
function mkDeck(d) {
  d.tabs = h('nav', { class: 'tabs' }, VIEWS.map(([id, label]) => h('button', { 'data-v': id, title: label, onclick: () => show(d, id) }, icon(id), h('span', null, label))));
  d.tabs.addEventListener('wheel', e => { if (!e.deltaY || e.shiftKey || d.tabs.scrollWidth <= d.tabs.clientWidth) return; d.tabs.scrollLeft += e.deltaY; e.preventDefault(); }, { passive: false });   // the wheel scrolls the tab row sideways
  d.body = h('div', { class: 'dbody' });
  d.el = h('section', { class: 'deck', id: 'deck' + d.id }, d.tabs, d.body);
}
function place(d, id) {
  d.cur = id; d.body.replaceChildren(views[id].el);
  [...d.tabs.children].forEach(b => b.classList.toggle('on', b.dataset.v === id));
  views[id].show && views[id].show();
  prefs['deck' + d.id] = id; savePrefs();
}
function show(d, id) {
  const other = active().find(o => o !== d && o.cur === id), prev = d.cur;
  place(d, id);
  if (other) place(other, prev && prev !== id ? prev : free());   // the two decks swap views
}
function layout() {
  const act = active();
  decksEl.className = narrow.matches ? 'single' : prefs.layout;
  decksEl.style.setProperty('--split', prefs.split);
  for (const d of DECKS) { const on = act.includes(d); d.el.classList.toggle('hide', !on); if (!on) d.cur = null; }
  for (const d of act) {
    const want = d.cur || prefs['deck' + d.id], taken = act.some(o => o !== d && o.cur === want);
    if (!d.cur || taken) place(d, views[want] && !taken ? want : free()); else views[d.cur].show && views[d.cur].show();
  }
  F.el.style.left = prefs.floatX + 'px'; F.el.style.top = prefs.floatY + 'px';
  document.getElementById('keys').classList.toggle('hide', !prefs.keys);
}
bus.on('goto', ({ to, from }) => {
  const act = active();
  if (act.some(d => d.cur === to)) return;
  const src = act.find(d => d.cur === from);
  show(act.length === 1 ? act[0] : act.find(d => d !== src && d === B) || act.find(d => d !== src), to);
});
// --- on-screen keyboard -----------------------------------------------------
// Docked under the decks (drag its top edge to resize), or floating in its own window-like panel (drag the left strip
// to move, the corner to resize). Its size and place are kept in prefs.keysUi.
function keysStrip() {
  const ui = prefs.keysUi ||= { h: 118, float: false, x: 120, y: 140, w: 760, fh: 170 };
  const box = h('div', { class: 'kbox' }), grip = h('div', { class: 'kgrip', title: 'Drag to resize the keyboard' });
  const floatB = btn(ui.float ? '⇲' : '⇱', () => { ui.float = !ui.float; savePrefs(); place(); }, 'sm', 'Dock the keyboard / make it a floating, resizable panel');
  const el = h('div', { id: 'keys', class: prefs.keys ? '' : 'hide' }, grip,
    h('div', { class: 'kside', title: ui.float ? 'Drag to move' : '' }, btn('+', () => shift(1), 'sm', 'Octave up'), h('small', null, ''), btn('−', () => shift(-1), 'sm', 'Octave down'), floatB), box);
  const shift = d => { prefs.octave = clamp(prefs.octave + d, 0, 7); savePrefs(); build(); };
  const down = new Map();
  function place() {
    el.classList.toggle('kfloat', !!ui.float); floatB.textContent = ui.float ? '⇲' : '⇱';
    if (ui.float) {
      ui.x = clamp(ui.x, 0, innerWidth - 200); ui.y = clamp(ui.y, 0, innerHeight - 80);
      Object.assign(el.style, { left: ui.x + 'px', top: ui.y + 'px', width: ui.w + 'px', height: ui.fh + 'px' });
    } else { Object.assign(el.style, { left: '', top: '', width: '', height: ui.h + 'px' }); }
    bus.emit('layout'); build();
  }
  // docked: drag the top edge; floating: drag the side strip to move
  grip.addEventListener('pointerdown', e => {
    e.preventDefault(); grip.setPointerCapture(e.pointerId); const y0 = e.clientY, h0 = el.offsetHeight;
    const mv = ev => { ui.h = clamp(h0 - (ev.clientY - y0), 60, Math.round(innerHeight * 0.5)); el.style.height = ui.h + 'px'; };
    const end = () => { grip.removeEventListener('pointermove', mv); savePrefs(); bus.emit('layout'); build(); };
    grip.addEventListener('pointermove', mv); grip.addEventListener('pointerup', end, { once: true });
  });
  el.querySelector('.kside').addEventListener('pointerdown', e => {
    if (!ui.float || e.target.closest('button')) return;
    e.preventDefault(); const side = e.currentTarget; side.setPointerCapture(e.pointerId); const dx = e.clientX - ui.x, dy = e.clientY - ui.y;
    const mv = ev => { ui.x = ev.clientX - dx; ui.y = ev.clientY - dy; el.style.left = ui.x + 'px'; el.style.top = ui.y + 'px'; };
    side.addEventListener('pointermove', mv); side.addEventListener('pointerup', () => { side.removeEventListener('pointermove', mv); savePrefs(); }, { once: true });
  });
  new ResizeObserver(() => { if (ui.float && el.offsetWidth) { const w = el.offsetWidth, fh = el.offsetHeight; if (w !== ui.w || fh !== ui.fh) { ui.w = w; ui.fh = fh; savePrefs(); } } build(); }).observe(el);
  let lastKey = '';
  function build() {
    // as many octaves as fit, with white keys about a third as wide as the keyboard is tall
    const wk = clamp((box.clientHeight || 110) * 0.3, 16, 64), octs = clamp(Math.floor((box.clientWidth || innerWidth) / (wk * 7)), 1, 8), base = (prefs.octave + 1) * 12;
    el.querySelector('small').textContent = 'C' + prefs.octave;
    if (lastKey === octs + ':' + base) return; lastKey = octs + ':' + base;
    box.replaceChildren();
    for (let n = base; n <= base + octs * 12; n++) {
      if (isBlack(n)) continue;
      const w = h('div', { class: 'wk', 'data-n': n }, n % 12 === 0 ? h('span', null, 'C' + (n / 12 - 1)) : null);
      if (isBlack(n + 1) && n < base + octs * 12) w.append(h('div', { class: 'bk', 'data-n': n + 1 }));
      box.append(w);
    }
  }
  requestAnimationFrame(place);
  bus.on('keysdock', place);
  const hit = e => { const t = document.elementFromPoint(e.clientX, e.clientY); if (!t || !t.dataset || !t.dataset.n || !box.contains(t)) return null; const r = t.getBoundingClientRect(); return [+t.dataset.n, clamp(0.35 + 0.65 * (e.clientY - r.top) / r.height, 0.2, 1)]; };
  box.addEventListener('pointerdown', e => { e.preventDefault(); box.setPointerCapture(e.pointerId); const k = hit(e); if (k) { down.set(e.pointerId, k[0]); engine.noteOn(k[0], k[1]); } });
  box.addEventListener('pointermove', e => { if (!down.has(e.pointerId)) return; const k = hit(e), cur = down.get(e.pointerId); if (k && k[0] !== cur) { engine.noteOff(cur); down.set(e.pointerId, k[0]); engine.noteOn(k[0], k[1]); } });
  const up = e => { if (down.has(e.pointerId)) { engine.noteOff(down.get(e.pointerId)); down.delete(e.pointerId); } };
  box.addEventListener('pointerup', up); box.addEventListener('pointercancel', up);
  bus.on('key', ({ note, on }) => { const k = box.querySelector(`[data-n="${note}"]`); if (k) k.classList.toggle('down', on); });
  return el;
}

// --- shortcuts ------------------------------------------------------------
const KMAP = { z: 0, s: 1, x: 2, d: 3, c: 4, v: 5, g: 6, b: 7, h: 8, n: 9, j: 10, m: 11, ',': 12, l: 13, '.': 14, q: 12, 2: 13, w: 14, 3: 15, e: 16, r: 17, 5: 18, t: 19, 6: 20, y: 21, 7: 22, u: 23, i: 24, 9: 25, o: 26, 0: 27, p: 28 };
const typed = new Map();
// Knobs with a `link` target get automation and MIDI-controller items in their right-click menu (util.knob).
knobHooks.items = (target, v) => {
  const links = linksFor(target), lane = S.p.song.auto.find(l => l.target === target);
  return ['-', { head: 'Automation' },
    { label: lane ? 'Show its automation lane' : 'Create automation lane', fn: () => { if (!lane) { S.p.song.auto.push({ id: uid(), target, pts: [{ t: 0, v }] }); engine.rememberParam(target); commit(); } bus.emit('goto', { to: 'song' }); } },
    { label: 'Start the song at this value', disabled: !lane, fn: () => { lane.pts = lane.pts.filter(p => p.t > 0); lane.pts.unshift({ t: 0, v }); commit(); } },
    { head: 'Remote control' },
    { label: 'Link to MIDI controller…', fn: () => learn(target) },
    ...links.map(l => ({ label: `Remove link (CC ${l.cc}, channel ${l.ch + 1})`, fn: () => unlink(target) }))];
};
function helpModal() {
  const rows = scope => ACTIONS.filter(a => a[3] === scope).map(a => h('div', { class: 'hk' }, h('kbd', null, keyOf(a[0])), h('span', null, a[1])));
  modal('Keyboard shortcuts', h('div', { class: 'hkhelp' }, ...Object.entries(SCOPES).flatMap(([k, t]) => [h('h4', null, t), ...rows(k)]),
    h('small', null, 'Change any of them in Settings > Hotkeys. Single-letter keys play notes while the typing keyboard is on; click the Song, Piano Roll, Rack or Mixer first to use their letters.')));
}
let undoMark = -1;   // Ctrl+Z toggles: pressed again right after an undo, it redoes
function shortcuts() {
  const goSet = tab => { bus.emit('goto', { to: 'set' }); setTimeout(() => bus.emit('settab', tab), 30); };
  const run = {
    play: () => engine.toggle(), pause: () => engine.pauseToggle(),
    undo: () => { if (undoMark === histPos()) { undo(1); undoMark = -1; } else { undo(-1); undoMark = histPos(); } }, undoStep: () => { undo(-1); undoMark = -1; },
    redo: () => undo(1), redo2: () => undo(1), save: saveFile, saveAs: () => T.saveAs(), saveNew: () => T.saveNew(), open: openFile,
    exportWav: () => exportWav(S.mode === 'song' ? 'song' : 'pat'), exportComp: () => exportComp(S.mode === 'song' ? 'song' : 'pat'), exportMidi,
    rec: () => T.recQuick(), metro: () => T.metro(), mode: () => T.mode(), time: () => T.time(), keys: () => T.keys(), prevPat: () => T.pat(-1), nextPat: () => T.pat(1),
    countIn: () => { prefs.countIn = !prefs.countIn; savePrefs(); toast(prefs.countIn ? 'Count-in on: one bar of clicks before recording' : 'Count-in off'); },
    waitInput: () => { prefs.waitInput = !prefs.waitInput; savePrefs(); toast(prefs.waitInput ? 'Recording waits for your first note' : 'Recording starts when you press play'); },
    panic: () => { if (engine.playing) engine.stop(); else engine.panic(); toast('All sound stopped'); },
    picker: () => choosePlugin(document.querySelector('#top .brand'), 'inst', pl => { const c = newChannel('plugin', { name: pl.name.slice(0, 18), plug: slotFor(pl, true) }); S.p.channels.push(c); S.ch = c.id; commit(); bus.emit('sel'); toast(`${pl.name} added to the rack`); }),
    midiSet: () => goSet('midi'), songInfo: () => goSet('project'), manual: helpModal,
    closeAll: () => { closeMenu(); document.querySelectorAll('.overlay').forEach(o => o.remove()); if (prefs.float) { prefs.float = false; savePrefs(); bus.emit('layout'); } if (prefs.keysUi && prefs.keysUi.float) { prefs.keysUi.float = false; savePrefs(); bus.emit('keysdock'); } },
    closeWin: () => { cancelLearn(); const ov = [...document.querySelectorAll('.overlay')].pop(); if (document.querySelector('.menu')) closeMenu(); else if (ov) ov.remove(); else if (prefs.float) { prefs.float = false; savePrefs(); bus.emit('layout'); } },
  };
  addEventListener('keydown', e => {
    const t = e.target, text = t.matches && t.matches('input:not([type=range]):not([type=checkbox]):not([type=color]),textarea'), mod = e.ctrlKey || e.metaKey || e.altKey, k = e.key.toLowerCase();
    if (text || (t.tagName === 'SELECT' && !mod)) return;
    let id = actionFor(e, 'global');
    if (id && !mod && !e.shiftKey && e.key.length === 1 && prefs.typing && KMAP[k] != null) id = null;   // a letter that plays a note while the typing keyboard is on
    if (id) {
      e.preventDefault();
      if (document.activeElement && document.activeElement.tagName === 'BUTTON') document.activeElement.blur();
      if (!e.repeat) { if (id.startsWith('v.')) bus.emit('goto', { to: id.slice(2) }); else if (run[id]) run[id](); }
    } else if (!mod && prefs.typing && !e.repeat && KMAP[k] != null && !typed.has(e.code)) { const n = (prefs.octave + 1) * 12 + KMAP[k]; typed.set(e.code, n); engine.noteOn(n, 0.8); }
  });
  addEventListener('keyup', e => { if (e.code === 'Space') e.preventDefault(); if (typed.has(e.code)) { engine.noteOff(typed.get(e.code)); typed.delete(e.code); } });
  addEventListener('blur', () => { for (const n of typed.values()) engine.noteOff(n); typed.clear(); });
}
async function boot() {
  applyPrefs();
  const ctx = engine.init();
  await initSamples(ctx, await buildKit()); await engine.ready;
  // Start empty unless Settings > Project says to reopen the last session; the previous session is kept aside either way.
  const saved = loadSaved();
  const tpl = String(prefs.startup).startsWith('tpl:') && fromTemplate(prefs.startup.slice(4));
  if (prefs.startup === 'last' && saved) setProject(saved, true); else { stashLast(saved); setProject(tpl || blankProject(), true); }
  for (const [id, , make] of VIEWS) views[id] = make();
  DECKS.forEach(mkDeck);
  const splitter = h('div', { class: 'splitter', title: 'Drag to resize' });
  splitter.addEventListener('pointerdown', e => {
    splitter.setPointerCapture(e.pointerId);
    const mv = ev => { const r = decksEl.getBoundingClientRect(); prefs.split = clamp(prefs.layout === 'side' ? (ev.clientX - r.left) / r.width : (ev.clientY - r.top) / r.height, 0.15, 0.85); decksEl.style.setProperty('--split', prefs.split); };
    const up = () => { splitter.removeEventListener('pointermove', mv); splitter.removeEventListener('pointerup', up); savePrefs(); };
    splitter.addEventListener('pointermove', mv); splitter.addEventListener('pointerup', up);
  });
  decksEl = h('main', { id: 'decks' }, A.el, splitter, B.el, DECKS[2].el, DECKS[3].el);
  // The floating deck: drag it by its tab bar, resize from its corner, close with the x.
  F.tabs.append(h('span', { class: 'grow' }), h('button', { class: 'fclose', title: 'Close the floating deck', onclick: () => { prefs.float = false; savePrefs(); layout(); } }, '✕'));
  F.tabs.addEventListener('pointerdown', e => {
    if (e.target.closest('button')) return;
    F.tabs.setPointerCapture(e.pointerId); const x0 = e.clientX - prefs.floatX, y0 = e.clientY - prefs.floatY;
    const mv = ev => { prefs.floatX = clamp(ev.clientX - x0, 0, innerWidth - 120); prefs.floatY = clamp(ev.clientY - y0, 0, innerHeight - 60); F.el.style.left = prefs.floatX + 'px'; F.el.style.top = prefs.floatY + 'px'; };
    const up = () => { F.tabs.removeEventListener('pointermove', mv); F.tabs.removeEventListener('pointerup', up); savePrefs(); };
    F.tabs.addEventListener('pointermove', mv); F.tabs.addEventListener('pointerup', up);
  });
  app.replaceChildren(topbar(), decksEl, keysStrip(), F.el);
  layout();
  bus.on('layout', layout); narrow.addEventListener('change', layout);
  shortcuts();
  const wake = () => engine.resume();
  addEventListener('pointerdown', wake, true); addEventListener('keydown', wake, true);
  midi.auto(); loadFolders();
  if ('serviceWorker' in navigator && location.protocol === 'https:' && location.hostname !== 'localhost') navigator.serviceWorker.register('sw.js').catch(() => {});
}
boot().catch(e => { app.replaceChildren(h('div', { id: 'boot' }, 'Undae Sonicae could not start: ' + (e && e.message || e))); console.error(e); });
