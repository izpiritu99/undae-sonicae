// Audio Lab: record or load audio, detect key/tempo, convert to MIDI, or drive an instrument live from the mic.
import { h, sel, btn, knob, bus, toast, pickFile, drawWave, cssVar, noteName, uid, STEP, BAR } from '../util.js';
import { S, pat, chan, commit, newChannel, fitBars } from '../state.js';
import { engine } from '../audio/engine.js';
import { addBuffer } from '../samples.js';
import { toMono, decimate, yin, detectKey, detectBpm, melodyNotes, chordNotes, toTicks, snapToScale } from '../audio/analysis.js';

const WORKLET = `registerProcessor('lw-rec', class extends AudioWorkletProcessor { process(i) { const c = i[0][0]; if (c) this.port.postMessage(c.slice(0)); return true; } });`;

export default function Lab() {
  const wave = h('canvas', { class: 'wave' }), info = h('small', null, 'Nothing loaded yet.'), tuner = h('div', { class: 'tuner' }, h('b', null, '–'), h('div', { class: 'cents' }, h('i')), h('small', null, 'Sing, hum or play a note'));
  const keyOut = h('div', { class: 'keyout' }), convOut = h('div', { class: 'convout' }), prog = h('progress', { max: 1, value: 0, class: 'hide' });
  const O = { mode: 'mono', gate: 0.08, minDur: 0.07, quant: STEP, align: true, replace: true, toKey: false, live: false };
  let audio = null, mic = null, found = null, key = null, pv = null, recBtn, liveBtn;

  const setAudio = (buffer, name) => { audio = { buffer, name, x: toMono(buffer), sr: buffer.sampleRate }; found = null; key = null; keyOut.replaceChildren(); convOut.replaceChildren(); paint(); };
  function paint() {
    info.textContent = audio ? `${audio.name} · ${audio.buffer.duration.toFixed(1)} s · ${audio.sr} Hz` : 'Nothing loaded yet.';
    requestAnimationFrame(() => wave.isConnected && drawWave(wave, audio && audio.x, cssVar('--accent')));
  }
  async function load() {
    const [f] = await pickFile('audio/*'); if (!f) return;
    try { setAudio(await engine.ctx.decodeAudioData(await f.arrayBuffer()), f.name.replace(/\.[^.]+$/, '')); } catch { toast('Could not decode that file'); }
  }
  // Start fresh: drop the loaded or recorded audio and everything worked out from it (also cancels a recording in progress).
  function clearAudio() {
    if (pv) { try { pv.stop(); } catch { /* ended */ } pv = null; }
    if (mic && mic.chunks) { mic.chunks = null; recBtn.classList.remove('on'); recBtn.textContent = '● Record mic'; closeMic(); }
    audio = null; found = null; key = null; keyOut.replaceChildren(); convOut.replaceChildren(); paint(); toast('Audio cleared');
  }
  function play() {
    if (pv) { try { pv.stop(); } catch { /* ended */ } pv = null; return; }
    if (!audio) return;
    engine.resume(); pv = engine.ctx.createBufferSource(); pv.buffer = audio.buffer; pv.connect(engine.ctx.destination); pv.onended = () => { pv = null; }; pv.start();
  }

  // --- microphone -------------------------------------------------------
  async function openMic() {
    if (mic) return mic;
    if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) { toast('Microphone input is not available here'); return null; }
    const ctx = engine.ctx; engine.resume();
    let stream;
    try { stream = await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false } }); }
    catch { toast('Microphone permission was denied'); return null; }
    const src = ctx.createMediaStreamSource(stream), an = ctx.createAnalyser(), mute = ctx.createGain(); an.fftSize = 2048; mute.gain.value = 0; src.connect(an); mute.connect(ctx.destination);
    mic = { stream, src, an, mute, chunks: null, buf: new Float32Array(2048), d: new Float32Array(400), cur: -1, cand: -1, n: 0 };
    try {
      await ctx.audioWorklet.addModule(URL.createObjectURL(new Blob([WORKLET], { type: 'application/javascript' })));
      mic.node = new AudioWorkletNode(ctx, 'lw-rec'); mic.node.port.onmessage = e => mic && mic.chunks && mic.chunks.push(e.data);
    } catch {
      mic.node = ctx.createScriptProcessor(4096, 1, 1); mic.node.onaudioprocess = e => mic && mic.chunks && mic.chunks.push(e.inputBuffer.getChannelData(0).slice(0));
    }
    src.connect(mic.node); mic.node.connect(mute);
    mic.timer = setInterval(track, 40);
    return mic;
  }
  function closeMic() {
    if (!mic || mic.chunks || O.live) return;
    clearInterval(mic.timer); mic.stream.getTracks().forEach(t => t.stop()); mic.src.disconnect(); mic.node.disconnect(); mic.mute.disconnect();
    if (mic.cur >= 0) engine.noteOff(mic.cur);
    mic = null; tuner.firstChild.textContent = '–';
  }
  // Live pitch tracker: updates the tuner and (optionally) plays the selected channel like a MIDI keyboard would.
  function track() {
    if (!mic) return;
    mic.an.getFloatTimeDomainData(mic.buf);
    const { x, sr } = decimate(mic.buf, engine.ctx.sampleRate, 11025), W = 256, tMax = Math.min(Math.ceil(sr / 70), x.length - W - 2);
    let rms = 0; for (let i = 0; i < x.length; i++) rms += x[i] * x[i]; rms = Math.sqrt(rms / x.length);
    const tau = rms > 0.012 ? yin(x, 0, W, Math.max(2, Math.floor(sr / 1500)), tMax, mic.d) : 0, m = tau ? 69 + 12 * Math.log2(sr / tau / 440) : -1, n = m < 0 ? -1 : Math.round(m);
    if (el.isConnected) { tuner.firstChild.textContent = n < 0 ? '–' : noteName(n); tuner.children[1].firstChild.style.transform = `translateX(${n < 0 ? 0 : (m - n) * 100}%)`; }
    if (!O.live) return;
    if (n === mic.cand) mic.n++; else { mic.cand = n; mic.n = 1; }
    if (mic.n >= 2 && n !== mic.cur) { if (mic.cur >= 0) engine.noteOff(mic.cur); if (n >= 0) engine.noteOn(n, Math.min(1, 0.4 + rms * 4)); mic.cur = n; }
  }
  async function toggleRec() {
    if (mic && mic.chunks) {
      const chunks = mic.chunks; mic.chunks = null; recBtn.classList.remove('on'); recBtn.textContent = '● Record mic';
      const len = chunks.reduce((s, c) => s + c.length, 0);
      if (len > engine.ctx.sampleRate * 0.2) { const b = engine.ctx.createBuffer(1, len, engine.ctx.sampleRate), d = b.getChannelData(0); let o = 0; for (const c of chunks) { d.set(c, o); o += c.length; } setAudio(b, 'Recording'); }
      closeMic(); return;
    }
    if (!(await openMic())) return;
    mic.chunks = []; recBtn.classList.add('on'); recBtn.textContent = '■ Stop recording';
  }
  async function toggleLive() {
    if (O.live) { O.live = false; liveBtn.classList.remove('on'); if (mic && mic.cur >= 0) { engine.noteOff(mic.cur); mic.cur = -1; } closeMic(); return; }
    if (!(await openMic())) return;
    O.live = true; liveBtn.classList.add('on'); toast('Live audio → MIDI is on. Use headphones to avoid feedback; arm ● REC and press play to capture it.', 5000);
  }

  // --- analysis ---------------------------------------------------------
  const busy = async fn => { prog.classList.remove('hide'); prog.value = 0; try { return await fn(v => { prog.value = v; }); } finally { prog.classList.add('hide'); } };
  async function analyse() {
    if (!audio) return toast('Record or load some audio first');
    key = await busy(p => detectKey(audio.x, audio.sr, p));
    const bpm = detectBpm(audio.x, audio.sr);
    keyOut.replaceChildren(
      h('div', { class: 'keybig' }, h('b', null, key.name), h('span', { class: 'tag' }, 'Camelot ' + key.camelot), h('span', { class: 'tag' }, Math.round(key.conf * 100) + '% confidence'), bpm ? h('span', { class: 'tag' }, '≈ ' + bpm + ' BPM') : null),
      h('div', { class: 'chroma' }, key.chroma.map((v, i) => h('div', { title: noteName(i).slice(0, -2) }, h('i', { style: `height:${Math.round(v * 100)}%` }), h('small', null, noteName(i).replace(/-?\d+$/, ''))))),
      h('small', null, 'Also possible: ' + key.alts.map(a => a.name).join(', ')),
      h('div', { class: 'hrow' },
        btn('Use as project key', () => { S.p.key = { root: key.root, scale: key.minor ? 'minor' : 'major' }; commit(); toast('Piano roll now highlights ' + key.name); }, 'primary'),
        bpm ? btn(`Set tempo to ${bpm}`, () => { engine.setBpm(bpm); commit(); }) : null));
  }
  async function convert() {
    if (!audio) return toast('Record or load some audio first');
    const fn = O.mode === 'mono' ? melodyNotes : chordNotes;
    found = await busy(p => fn(audio.x, audio.sr, { gate: O.gate, minDur: O.minDur }, p));
    const cv = h('canvas', { class: 'wave' });
    convOut.replaceChildren(h('b', null, found.length ? `${found.length} notes found` : 'No notes found - try lowering Sensitivity'), cv,
      found.length ? h('div', { class: 'hrow' }, btn('Insert into piano roll', insert, 'primary'), h('small', null, `Goes to “${(chan() || { name: 'a channel you add first' }).name}” in “${pat().name}” at ${S.p.bpm} BPM`)) : '');
    requestAnimationFrame(() => {
      if (!found.length || !cv.isConnected) return;
      const dpr = devicePixelRatio || 1, w = cv.clientWidth, hh = cv.clientHeight, g = cv.getContext('2d'); cv.width = w * dpr; cv.height = hh * dpr; g.scale(dpr, dpr);
      const lo = Math.min(...found.map(n => n.n)) - 1, hi = Math.max(...found.map(n => n.n)) + 1, dur = audio.buffer.duration, rh = Math.min(8, hh / (hi - lo + 1));
      g.fillStyle = cssVar('--accent');
      for (const n of found) { g.globalAlpha = 0.4 + 0.6 * n.v; g.fillRect(n.s / dur * w, (hi - n.n) / (hi - lo + 1) * hh, Math.max(2, (n.e - n.s) / dur * w - 1), Math.max(2, rh - 1)); }
    });
  }
  function insert() {
    const c = chan(), p = pat(); if (!c) return toast('Add a channel in the Rack first'); let ns = toTicks(found, S.p.bpm, O.quant, O.align);
    if (O.toKey && S.p.key) ns = ns.map(n => ({ ...n, n: snapToScale(n.n, S.p.key) }));
    const before = ns.length; ns = ns.filter(n => n.t < 64 * BAR);
    const keep = O.replace ? [] : p.notes[c.id] || [];
    p.notes[c.id] = [...keep, ...ns.map(n => ({ ...n, id: uid() }))];
    p.bars = Math.max(p.bars, fitBars(ns.reduce((m, n) => Math.max(m, n.t + n.d), 0)));
    commit(); bus.emit('sel'); bus.emit('goto', { to: 'roll', from: 'lab' });
    toast(before > ns.length ? `Inserted ${ns.length} notes (audio longer than 64 bars was trimmed)` : `Inserted ${ns.length} notes`);
  }
  async function asSample() {
    if (!audio) return toast('Record or load some audio first');
    const id = await addBuffer(audio.name, audio.buffer), c = newChannel('sample', { name: audio.name.slice(0, 18), sample: id, gate: true, rel: 0.2 });
    S.p.channels.push(c); S.ch = c.id; commit(); bus.emit('goto', { to: 'inst', from: 'lab' });
  }

  const check = (k, label) => h('label', { class: 'chk' }, h('input', { type: 'checkbox', checked: O[k], onchange: e => { O[k] = e.target.checked; } }), label);
  const card = (title, ...kids) => h('section', { class: 'card' }, h('h4', null, title), kids);
  const el = h('div', { class: 'view lab' }, h('div', { class: 'cards' },
    card('1 · Audio in',
      h('div', { class: 'hrow' }, recBtn = btn('● Record mic', toggleRec, 'rec'), btn('Load audio file…', load), btn('▶ / ■', play, 'sm', 'Preview'), btn('Add as instrument', asSample, 'sm', 'Use this audio as a playable sampler channel'), btn('✕ Clear', clearAudio, 'sm', 'Remove this audio and its results so you can record or load something new')),
      wave, info, prog),
    card('Live pitch',
      tuner, h('div', { class: 'hrow' }, liveBtn = btn('Live audio → MIDI', toggleLive, '', 'Play the selected channel with your voice or instrument; records like a MIDI keyboard'),
        h('small', null, 'Tracks one note at a time from the microphone.'))),
    card('2 · Key & tempo', h('div', { class: 'hrow' }, btn('Detect key & tempo', analyse, 'primary'), h('small', null, 'Works on full songs, loops or a hummed tune.')), keyOut),
    card('3 · Audio → MIDI',
      h('div', { class: 'krow' },
        h('label', { class: 'pick' }, 'Source type', sel([['mono', 'Melody (one note at a time)'], ['poly', 'Chords (experimental)']], O.mode, v => { O.mode = v; })),
        knob({ label: 'Sensitivity', size: 'sm', min: 0.01, max: 0.4, curve: 'exp', value: O.gate, def: 0.08, fmt: v => Math.round((1 - Math.log(v / 0.01) / Math.log(40)) * 100) + '%', onInput: v => { O.gate = v; } }),
        knob({ label: 'Min note', size: 'sm', min: 0.03, max: 0.4, curve: 'exp', value: O.minDur, def: 0.07, fmt: v => Math.round(v * 1000) + 'ms', onInput: v => { O.minDur = v; } }),
        h('label', { class: 'pick' }, 'Quantize', sel([[0, 'Off'], [STEP * 2, '1/8'], [STEP, '1/16'], [STEP / 2, '1/32']], O.quant, v => { O.quant = +v; }))),
      h('div', { class: 'hrow' }, check('align', 'Start at bar 1'), check('replace', 'Replace existing notes'), check('toKey', 'Snap to project key')),
      h('div', { class: 'hrow' }, btn('Convert to MIDI', convert, 'primary')), convOut)));
  bus.on('theme', paint);
  return { el, show: paint };
}
