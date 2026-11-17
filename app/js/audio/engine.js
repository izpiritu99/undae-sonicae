// Audio graph (mixer buses + channel strips), look-ahead sequencer, live note input and offline rendering.
import { S, pat, chan, prefs, songBars, commit, notesOf, reaches } from '../state.js';
import { samples, addBuffer } from '../samples.js';
import { makeFx, loadFxWorklets, FX, offlineAudio } from './fx.js';
import { renderOffline } from './bridge.js';
import { synthVoice, sampleVoice } from './synth.js';
import { bus, PPQ, STEP, BAR, BEAT, uid, toast, clamp } from '../util.js';
import { stretch } from './stretch.js';

const stretched = new Map();   // time-stretched copies of recorded takes, keyed by sample + ratio

const TAP = `registerProcessor('us-tap', class extends AudioWorkletProcessor { process(i) { const a = i[0]; if (a && a[0]) this.port.postMessage([a[0].slice(0), (a[1] || a[0]).slice(0)]); return true; } });`;
// Swing as a time warp over each pair of grid cells: the off-beat lands late and everything around it stretches to match.
export function swingShift(P, t) {
  if (!P.swing) return 0;
  const g = P.swingDiv === 8 ? STEP * 2 : STEP, s = P.swing * g * 0.66, x = t % (2 * g);
  return x < g ? x * s / g : s * (2 * g - x) / g;
}

const sigOf = P => P.buses.map(b => b.id + ':' + b.fx.map(f => f.id).join(',') + '>' + b.sends.map(s => s.to).join('+')).join('|');

class Graph {
  constructor(ctx, live) { this.ctx = ctx; this.live = live; this.buses = new Map(); this.strips = new Map(); this.voices = new Map(); this.last = new Map(); this.build(); }
  set(p, v) { if (this.live && this.ctx.state === 'running') p.setTargetAtTime(v, this.ctx.currentTime, 0.012); else p.value = v; }
  build() {
    const { ctx } = this, P = S.p;
    for (const b of this.buses.values()) { b.out.disconnect(); b.fx.forEach(f => f.dispose && f.dispose()); }
    this.keep = new Map();                                                // plug-in instruments survive a rebuild: reloading one is slow and audible
    for (const [id, s] of this.strips) { s.pan.disconnect(); if (s.plug) this.keep.set(id, s.plug); }
    this.buses.clear(); this.strips.clear();
    const nodes = P.buses.map(b => this.mkBus(b)), master = nodes[0];
    master.out.connect(ctx.destination);
    // Every bus feeds the master through its own link (which can be switched off) and any number of sends to other buses.
    P.buses.forEach((b, i) => {
      const n = nodes[i];
      if (i) { n.out.connect(n.toM); n.toM.connect(master.inp); }
      n.sendG = b.sends.map(s => { const g = ctx.createGain(), t = this.buses.get(s.to); n.out.connect(g); if (t && t !== master && t !== n && !reaches(P, s.to, b.id)) g.connect(t.inp); return g; });
    });
    this.sig = sigOf(P);
    this.sync();
  }
  mkBus(b) {
    const { ctx } = this, g = () => ctx.createGain();
    const n = { inp: g(), inv: g(), ll: g(), lr: g(), rl: g(), rr: g(), fxIn: g(), fxOut: g(), pan: ctx.createStereoPanner(), fader: g(), out: g(), toM: g(), sendG: [], chainKey: null };
    n.inp.channelCount = 2; n.inp.channelCountMode = 'explicit';
    const sp = ctx.createChannelSplitter(2), mg = ctx.createChannelMerger(2);
    n.inp.connect(n.inv); n.inv.connect(sp);
    sp.connect(n.ll, 0); n.ll.connect(mg, 0, 0); sp.connect(n.lr, 0); n.lr.connect(mg, 0, 1);
    sp.connect(n.rl, 1); n.rl.connect(mg, 0, 0); sp.connect(n.rr, 1); n.rr.connect(mg, 0, 1);
    mg.connect(n.fxIn); n.fxOut.connect(n.pan); n.pan.connect(n.fader); n.fader.connect(n.out);
    if (this.live) { n.an = ctx.createAnalyser(); n.an.fftSize = 512; n.out.connect(n.an); n.buf = new Float32Array(512); }
    n.fx = b.fx.map(f => makeFx(ctx, f.type, f.p, this.live));
    b.fx.forEach((f, i) => { if (f.type === 'plugin') n.fx[i].autoKey = 'fx:' + b.id + ':' + f.id + ':'; });   // automation target prefix for its parameters
    this.buses.set(b.id, n);
    return n;
  }
  update() { if (sigOf(S.p) !== this.sig) this.build(); else this.sync(); }
  // Push every mixer/channel parameter from the project into the node graph (cheap; safe to call on every tweak).
  sync() {
    const P = S.p, { ctx } = this, anySolo = P.buses.some(b => b.solo);
    for (const b of P.buses) {
      const n = this.buses.get(b.id); if (!n) continue;
      const w = b.mono ? 0 : b.width;
      this.set(n.inv.gain, b.inv ? -1 : 1);
      this.set(n.ll.gain, (1 + w) / 2); this.set(n.rr.gain, (1 + w) / 2); this.set(n.lr.gain, (1 - w) / 2); this.set(n.rl.gain, (1 - w) / 2);
      this.set(n.pan.pan, b.pan); this.set(n.fader.gain, b.vol);
      this.set(n.out.gain, b.mute || (anySolo && !b.solo && b.kind === 'insert') ? 0 : 1);
      this.set(n.toM.gain, b.toMaster === false ? 0 : 1); b.sends.forEach((s, i) => n.sendG[i] && this.set(n.sendG[i].gain, s.amt));
      const key = b.fx.map(f => (f.on ? 1 : 0)).join('');
      if (n.chainKey !== key) {
        n.chainKey = key; n.fxIn.disconnect(); n.fx.forEach(x => x.out.disconnect());
        let cur = n.fxIn;
        b.fx.forEach((f, i) => { if (f.on) { cur.connect(n.fx[i].inp); cur = n.fx[i].out; } });
        cur.connect(n.fxOut);
      }
      b.fx.forEach((f, i) => { if (n.fx[i].bind) n.fx[i].bind(f.p); for (const k in f.p) n.fx[i].set(k, f.p[k]); });
    }
    // Peak controllers listen to another bus: keep that side-chain wire pointing at the chosen source.
    for (const b of P.buses) b.fx.forEach((f, i) => {
      const x = this.buses.get(b.id).fx[i]; if (!x.side || x._src === f.p.src) return;
      if (x._node) { try { x._node.disconnect(x.side); } catch { /* gone */ } }
      const sn = f.p.src && f.p.src !== b.id ? this.buses.get(f.p.src) : null;
      x._node = sn ? sn.fxOut : null; x._src = f.p.src; if (sn) sn.fxOut.connect(x.side);
    });
    const chSolo = P.channels.some(c => c.solo), seen = new Set(), master = this.buses.get(P.buses[0].id);
    for (const c of P.channels) {
      seen.add(c.id);
      let s = this.strips.get(c.id);
      if (!s) { s = { g: ctx.createGain(), pan: ctx.createStereoPanner(), bus: null }; s.g.connect(s.pan); this.strips.set(c.id, s); }
      if (c.type === 'plugin') {                                           // a plug-in instrument: its host connection feeds this strip
        if (!s.plug) { s.plug = (this.keep && this.keep.get(c.id)) || makeFx(ctx, 'plugin', c.plug, this.live); if (this.keep) this.keep.delete(c.id); try { s.plug.out.disconnect(); } catch { /* new */ } s.plug.out.connect(s.g); }
        s.plug.autoKey = 'plug:' + c.id + ':'; s.plug.bind(c.plug); for (const k in c.plug) if (k[0] === 'v' || k === 'path' || k === 'pid') s.plug.set(k, c.plug[k]);
      }
      const target = this.buses.get(c.bus) || master;
      if (s.bus !== target) { s.pan.disconnect(); s.pan.connect(target.inp); s.bus = target; }
      this.set(s.g.gain, c.mute || (chSolo && !c.solo) ? 0 : c.vol); this.set(s.pan.pan, c.pan);
    }
    for (const [id, s] of this.strips) if (!seen.has(id)) { s.pan.disconnect(); if (s.plug) s.plug.dispose(); this.strips.delete(id); }
    if (this.keep) { for (const x of this.keep.values()) x.dispose(); this.keep = null; }
  }
  // Start an audio clip (a recording placed on the song timeline) on its bus. off/dur in seconds.
  playClip(a, buffer, t, off, dur) {
    const smp = buffer && { buffer }, target = this.buses.get(a.bus) || this.buses.get(S.p.buses[0].id);
    if (!smp || dur <= 0.005) return null;
    const s = this.ctx.createBufferSource(), g = this.ctx.createGain(); s.buffer = smp.buffer;
    g.gain.setValueAtTime(0, t); g.gain.linearRampToValueAtTime(a.gain ?? 1, t + 0.004); g.gain.setValueAtTime(a.gain ?? 1, t + Math.max(0.005, dur - 0.006)); g.gain.linearRampToValueAtTime(0, t + dur);
    s.connect(g); g.connect(target.inp); s.start(t, off, dur);
    s.onended = () => { try { g.disconnect(); } catch { /* gone */ } };
    return s;
  }
  // o: { bend (cents), wheel, glideFrom, fine (cents, per note), pan (-1..1, per note), rel / cut / res (-1..1 around the
  // channel's own release, cutoff and resonance), slide }. The master pitch applies to all.
  // A slide note starts no voice of its own: the channel's sounding notes glide to its pitch over its length (as in FL Studio).
  // With nothing sounding it plays as an ordinary note.
  trigger(c, note, vel, t, dur, o = {}) {
    const s = this.strips.get(c.id); if (!s) return null;
    const mp = S.p.masterPitch || 0;
    if (mp || o.fine) o = { ...o, bend: (o.bend || 0) + mp * 100 + (o.fine || 0) };
    let list = this.voices.get(c.id); if (!list) this.voices.set(c.id, (list = []));
    for (let i = list.length - 1; i >= 0; i--) if (list[i].end < t) list.splice(i, 1);
    if (o.slide) { const live = list.filter(x => x.slide && x.t < t && x.end > t && !x.released); if (live.length) { live.forEach(x => x.slide(note, t, dur || 0.05)); return null; } }
    if (c.choke) for (const oc of S.p.channels) if (oc.choke === c.choke && oc.id !== c.id) (this.voices.get(oc.id) || []).forEach(v => v.t <= t && v.end > t && v.kill(t));
    let v;
    if (c.type === 'plugin') {                                             // notes go to the host, stamped with their time
      const x = s.plug; if (!x || !x.info) return null;
      note = clamp(note + Math.round(mp), 0, 127);   // plug-ins: master pitch in whole semitones
      // per-note cutoff / resonance as the standard brightness / timbre controllers (CC74 / CC71); back to the centre after
      const cc = (n, val) => { const k = Math.round(clamp(64 + (val || 0) * 63, 0, 127)), m = (x.cc ||= {}); if (m[n] !== k && (k !== 64 || m[n] != null)) { x.midi([0xB0, n, k], t); m[n] = k; } };
      cc(74, o.cut); cc(71, o.res);
      x.note(true, note, vel, t); if (dur != null) x.note(false, note, 0, t + dur);
      v = { t, end: dur == null ? Infinity : t + dur, release(tr) { x.note(false, note, 0, tr); v.end = Math.min(v.end, tr); }, kill(tk) { v.release(tk); } };
      list.push(v); return v;
    }
    let dest = s.g;
    if (o.pan) { const pn = this.ctx.createStereoPanner(); pn.pan.value = clamp(o.pan, -1, 1); pn.connect(s.g); dest = pn; if (dur != null) setTimeout(() => { try { pn.disconnect(); } catch { /* gone */ } }, (t - this.ctx.currentTime + dur + 8) * 1000); }
    if (c.type === 'synth') {
      const P = c.synth;
      if (P.mode !== 'poly') list.forEach(x => x.t <= t && x.end > t && x.kill(t));
      else { const max = Math.min(P.voices, prefs.maxVoices); while (list.length >= max) list.shift().kill(t); }
      v = synthVoice(this.ctx, P, note, vel, t, dur, dest, { glideFrom: this.last.get(c.id), bpm: S.p.bpm, ...o });
      this.last.set(c.id, note);
    } else {
      const smp = samples.get(c.sample); if (!smp) return null;
      if (c.cut) list.forEach(x => x.t <= t && x.end > t && x.kill(t));
      while (list.length >= 24) list.shift().kill(t);
      v = sampleVoice(this.ctx, smp.buffer, c, note, vel, t, dur, dest, o);
    }
    list.push(v);
    return v;
  }
}

// The per-note settings a collected note hands to trigger().
export const noteOpts = e => ({ fine: e.fine, pan: e.pan, rel: e.rel, cut: e.cut, res: e.res, slide: e.slide });
// Gather every note that starts in the linear tick window [a, b), looping the pattern or the song.
export function collect(P, mode, pt, a, b, once) {
  const out = [], solo = P.channels.some(c => c.solo), chans = P.channels.filter(c => !(c.mute || (solo && !c.solo)));
  const add = (p, base, limit, lo = -Infinity) => {
    const pl = p.bars * BAR;
    for (const c of chans) {
      const ns = p.notes[c.id]; if (!ns) continue;
      for (const n of ns) {
        if (n.mute) continue;   // muted in the Piano Roll
        const tk = base + n.t;
        if (tk >= a && tk >= lo && tk < b && tk < limit && n.t < pl) out.push({ c, n: n.n, v: n.v, d: Math.min(n.d, limit - tk), tick: tk, sw: swingShift(P, n.t), pan: n.pan, fine: n.fine, rel: n.rel, cut: n.cut, res: n.res, slide: n.slide });
      }
    }
  };
  if (mode !== 'song') {
    const lp = pt.loop && pt.loop.on && pt.loop.b > pt.loop.a ? pt.loop : null;   // Piano Roll loop: play only that part, over and over
    if (lp) {
      const W = lp.b - lp.a, sub = { ...pt, notes: {} };
      for (const id in pt.notes) sub.notes[id] = pt.notes[id].filter(n => n.t >= lp.a && n.t < lp.b).map(n => ({ ...n, t: n.t - lp.a, d: Math.min(n.d, lp.b - n.t) }));
      sub.bars = W / BAR;
      for (let k = Math.floor(a / W); k * W < b; k++) add(sub, k * W, (k + 1) * W);
      return out;
    }
    const L = pt.bars * BAR; for (let k = Math.floor(a / L); k * L < b; k++) add(pt, k * L, Infinity); return out;
  }
  const L = songBars(P) * BAR; if (!L) return out;
  if (once && a >= L) return out;
  for (let k = Math.floor(a / L); k * L < b && !(once && k); k++) for (const cl of P.song.clips) {
    const p = P.patterns.find(x => x.id === cl.pat); if (!p || cl.mute) continue;
    const cs = k * L + Math.round(cl.start * BAR), ce = cs + Math.round(cl.len * BAR), pl = p.bars * BAR, off = Math.round((cl.off || 0) * BAR) % pl;   // off: slipped content
    if (ce <= a || cs >= b) continue;
    for (let m = Math.max(0, Math.floor((a - cs + off) / pl)); cs - off + m * pl < Math.min(b, ce); m++) add(p, cs - off + m * pl, ce, cs);
  }
  return out;
}

let ctx;
export const engine = {
  ctx: null, graph: null, playing: false, aTick: 0, aTime: 0, next: 0, held: new Map(), recStart: new Map(), sustain: false, sus: new Map(), bend: 0, wheel: 0, recDirty: false,
  inputs: new Map(), recs: [], clipSrc: new Set(), recAudio: false, recLinear: false, writing: new Map(), autoDirty: false,
  init() {
    const o = { latencyHint: prefs.latency }; if (prefs.sampleRate) o.sampleRate = prefs.sampleRate;
    try { ctx = new AudioContext(o); } catch { ctx = new AudioContext(); }
    this.ctx = ctx;
    if (prefs.sinkId) this.setSink(prefs.sinkId);
    this.ready = loadFxWorklets(ctx);
    try { new Worker(URL.createObjectURL(new Blob(['setInterval(()=>postMessage(0),25)']))).onmessage = () => this.schedule(); }
    catch { setInterval(() => this.schedule(), 25); }
    return ctx;
  },
  // Route the whole app to a specific output device (speakers, interface, JACK/PipeWire sink...). '' = system default.
  async setSink(id) { if (!ctx.setSinkId) return false; try { await ctx.setSinkId(id || ''); return true; } catch { return false; } },
  reloadPlugins() { const g = this.graph; if (!g) return; for (const s of g.strips.values()) if (s.plug && s.plug.reload) s.plug.reload(); for (const b of g.buses.values()) for (const f of b.fx || []) if (f && f.reload) f.reload(); },
  refresh() { if (!ctx || !S.p) return; if (!this.graph) this.graph = new Graph(ctx, true); else this.graph.update(); this.wireInputs(); },
  resume() { if (ctx.state !== 'running') ctx.resume(); },
  spt() { return 60 / S.p.bpm / PPQ; },
  loopLen() { if (S.mode === 'song') return songBars(S.p) * BAR; const p = pat(), lp = p.loop; return lp && lp.on && lp.b > lp.a ? lp.b - lp.a : p.bars * BAR; },
  voiceCount() { const t = this.ctx ? this.ctx.currentTime : 0; let n = 0; if (this.graph) for (const l of this.graph.voices.values()) for (const v of l) if (v.end > t) n++; return n; },
  tickNow() { return Math.max(this.aTick, this.aTick + (ctx.currentTime - this.aTime) / this.spt()); },
  pos() { if (this.recLinear) return this.tickNow(); const L = this.loopLen(); return L ? this.tickNow() % L : 0; },
  // from: start tick (play / pause, play-from-click). Recording can wait for the first note, and can count in one bar.
  play(from, force) {
    if (S.rec && prefs.waitInput && !force && !this.playing) { this.waiting = { from }; toast('Waiting for you to play a note…'); bus.emit('transport'); return; }
    this.waiting = null;
    this.resume(); this.playing = true;
    const L = this.loopLen(); let st = S.mode === 'song' && S.songStart * BAR < L ? S.songStart * BAR : S.mode === 'song' && S.rec && this.inputs.size ? S.songStart * BAR : 0;
    if (from != null) st = clamp(from, 0, Math.max(0, L - 1));
    if (S.mode === 'song') for (const a of S.p.song.audio) this.clipBuf(a);   // stretch takes to the current tempo before the clock starts
    this.aTick = this.next = st; this.aTime = ctx.currentTime + 0.06;
    if (S.rec && prefs.countIn) {   // one bar of clicks before the music (and the recording) starts
      const beat = BEAT * this.spt(), n = Math.round(BAR / BEAT);
      for (let i = 0; i < n; i++) this.click(this.aTime + i * beat, i === 0);
      this.aTime += n * beat;
    }
    this.recAudio = S.rec && prefs.recWhat.audio && S.mode === 'song' && this.inputs.size > 0;
    this.recLinear = this.recAudio && !(prefs.loopRec && L > 0);   // loop recording keeps cycling and stacks takes; otherwise the take runs on past the end
    if (S.rec && prefs.recWhat.audio && S.mode !== 'song' && this.inputs.size) toast('Audio records in SONG mode - switch the PAT / SONG toggle to SONG');
    if (S.mode === 'song') this.startClips(st);
    if (this.recAudio) this.beginRec(st);
    this.schedule(); bus.emit('transport');
  },
  stop() {
    this.playing = false; this.panic();
    for (const s of this.clipSrc) { try { s.stop(); } catch { /* ended */ } } this.clipSrc.clear();
    if (this.recAudio) this.finishRec();
    this.flushAuto();
    if (this.recDirty) { this.recDirty = false; commit('notes'); }
    bus.emit('transport');
  },
  toggle() { if (this.waiting) { this.waiting = null; bus.emit('transport'); return; } if (this.playing) { this.resumeAt = undefined; this.stop(); } else this.play(); },
  // Play / pause: stopping remembers the position, and the next start carries on from it.
  pauseToggle() { if (this.playing) { this.resumeAt = this.pos(); this.stop(); } else this.play(this.resumeAt); },
  panic() {
    const t = ctx.currentTime;
    for (const l of this.graph.voices.values()) { l.forEach(v => v.kill(t)); l.length = 0; }
    for (const s of this.graph.strips.values()) if (s.plug) s.plug.panic();   // plug-in instruments: notes queued ahead and long releases too
    this.held.clear(); this.recStart.clear(); this.sus.clear();
  },
  setBpm(v) { if (this.playing) { const tk = this.tickNow(); this.aTick = tk; this.aTime = ctx.currentTime; } S.p.bpm = v; },
  schedule() {
    if (!this.playing) return;
    const spt = this.spt(), now = ctx.currentTime, to = this.aTick + (now + prefs.lookahead - this.aTime) / spt;
    if (to <= this.next) return;
    if (S.rec && !prefs.loopRec && S.mode !== 'song' && this.tickNow() >= this.loopLen()) { this.stop(); return; }   // loop record off: one pass, then stop
    this.applyAuto(this.pos());
    for (const e of collect(S.p, S.mode, pat(), this.next, to, this.recLinear)) this.graph.trigger(e.c, e.n, e.v, Math.max(now, this.aTime + (e.tick + e.sw - this.aTick) * spt), e.d * spt, noteOpts(e));
    if (S.mode === 'song') {
      const L = songBars(S.p) * BAR;
      if (L) for (let k = Math.floor(this.next / L); k * L < to && !(this.recLinear && k); k++) for (const a of S.p.song.audio) {
        const cs = k * L + Math.round(a.start * BAR);
        if (cs >= this.next && cs < to) this.startClip(a, this.aTime + (cs - this.aTick) * spt, 0);
      }
    }
    if (S.metro) for (let b = Math.ceil(this.next / BEAT); b * BEAT < to; b++) this.click(this.aTime + (b * BEAT - this.aTick) * spt, (b * BEAT) % BAR === 0);
    this.next = to;
  },
  // --- audio clips on the song timeline ------------------------------------
  // How much longer (>1) or shorter a take must play to follow the project tempo it was not recorded at.
  ratio(a) { return a.stretch === false || !a.bpm ? 1 : clamp(a.bpm / S.p.bpm, 0.25, 4); },
  clipBuf(a) {
    const smp = samples.get(a.sample); if (!smp) return null;
    const r = this.ratio(a); if (Math.abs(r - 1) < 0.002) return smp.buffer;
    const key = a.sample + ':' + r.toFixed(4); let b = stretched.get(key);
    if (!b) { if (stretched.size > 24) stretched.clear(); b = stretch(ctx, smp.buffer, r); stretched.set(key, b); }
    return b;
  },
  clipDur(a) { const smp = samples.get(a.sample); return smp ? Math.min(a.len * BAR * this.spt(), (smp.buffer.duration - (a.off || 0)) * this.ratio(a)) : 0; },
  startClip(a, t, skip) {
    if (a.mute) return;
    const s = this.graph.playClip(a, this.clipBuf(a), Math.max(ctx.currentTime, t), (a.off || 0) * this.ratio(a) + skip, this.clipDur(a) - skip);
    if (s) { this.clipSrc.add(s); s.addEventListener('ended', () => this.clipSrc.delete(s)); }
  },
  startClips(st) {
    for (const a of S.p.song.audio) { const cs = Math.round(a.start * BAR); if (cs < st && cs + a.len * BAR > st) this.startClip(a, this.aTime, (st - cs) * this.spt()); }
  },
  // --- audio inputs: one microphone / line input per armed mixer bus ---------
  async tapNode() {
    this.tapReady ||= ctx.audioWorklet.addModule(URL.createObjectURL(new Blob([TAP], { type: 'application/javascript' })));
    await this.tapReady;
    return new AudioWorkletNode(ctx, 'us-tap');
  },
  async arm(b) {
    this.disarm(b);
    if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) { toast('Audio input is not available here'); return false; }
    this.resume();
    let stream;
    const want = { echoCancellation: false, noiseSuppression: false, autoGainControl: false, channelCount: b.stereo ? 2 : 1 };
    const dev = b.input === 'default' ? prefs.inputId : b.input;   // 'default': the input chosen in Settings > Audio, or the system's
    if (dev) want.deviceId = { exact: dev };
    try { stream = await navigator.mediaDevices.getUserMedia({ audio: want }); }
    catch { toast('Could not open that input - permission denied or the device is missing'); return false; }
    const src = ctx.createMediaStreamSource(stream), mon = ctx.createGain(), sink = ctx.createGain(); sink.gain.value = 0; sink.connect(ctx.destination);
    const inp = { stream, src, mon, sink, rec: null, peak: 0 };
    const feed = (l, r) => this.onTap(inp, l, r);
    try { inp.tap = await this.tapNode(); inp.tap.port.onmessage = e => feed(e.data[0], e.data[1]); }
    catch { inp.tap = ctx.createScriptProcessor(2048, 2, 2); inp.tap.onaudioprocess = e => feed(e.inputBuffer.getChannelData(0).slice(0), e.inputBuffer.getChannelData(1).slice(0)); }
    src.connect(inp.tap); inp.tap.connect(sink); src.connect(mon);
    this.inputs.set(b.id, inp); b.arm = true; this.wireInputs();
    return true;
  },
  disarm(b) {
    const i = this.inputs.get(b.id);
    if (i) { i.stream.getTracks().forEach(t => t.stop()); try { i.src.disconnect(); i.mon.disconnect(); i.tap.disconnect(); } catch { /* gone */ } this.inputs.delete(b.id); }
    b.arm = false;
  },
  wireInputs() {
    if (!this.graph) return;
    for (const b of S.p.buses) b.arm = this.inputs.has(b.id);
    for (const [id, i] of this.inputs) {
      const b = S.p.buses.find(x => x.id === id), n = this.graph.buses.get(id);
      try { i.mon.disconnect(); } catch { /* not connected */ }
      if (!b || !n) { this.disarm({ id }); continue; }
      i.mon.gain.value = b.mon ? 1 : 0; i.mon.connect(n.inp);
    }
  },
  inLevel(id) { const i = this.inputs.get(id); if (!i) return 0; const p = i.peak; i.peak = 0; return p; },
  onTap(inp, l, r) {
    let m = 0; for (let k = 0; k < l.length; k += 4) { const a = Math.abs(l[k]); if (a > m) m = a; }
    if (m > inp.peak) inp.peak = m;
    const rec = inp.rec; if (!rec) return;
    if (rec.t1 == null) rec.t1 = ctx.currentTime - l.length / ctx.sampleRate;
    rec.L.push(l); if (rec.stereo) rec.R.push(r); rec.len += l.length;
    rec.acc = Math.max(rec.acc, m); rec.n += l.length;
    if (rec.n >= 1024) { rec.peaks.push(rec.acc); rec.acc = 0; rec.n = 0; }
  },
  beginRec(st) {
    const P = S.p, used = new Set(); let added = false;
    for (const [id, inp] of this.inputs) {
      const b = P.buses.find(x => x.id === id); if (!b) continue;
      let track = 0;
      const busy = tr => used.has(tr) || [...P.song.clips, ...P.song.audio].some(c => c.track === tr && (c.start + c.len) * BAR > st);
      while (track < P.song.tracks && busy(track)) track++;
      if (track >= P.song.tracks) { P.song.tracks = track + 1; added = true; }
      used.add(track);
      inp.rec = { bus: id, name: b.name, L: [], R: [], len: 0, peaks: [], acc: 0, n: 0, st, t1: null, track, stereo: !!b.stereo, sr: ctx.sampleRate, linear: this.recLinear };
      this.recs.push(inp.rec);
    }
    if (added) bus.emit('project');
  },
  // Where a take sits on the timeline: its first sample, pulled back by the round-trip latency.
  takeStart(rec) {
    const lat = (ctx.baseLatency || 0) + (ctx.outputLatency || 0) + prefs.recOffset / 1000, raw = rec.st + ((rec.t1 ?? this.aTime) - this.aTime - lat) / this.spt();
    return raw < rec.st ? { tick: rec.st, off: (rec.st - raw) * this.spt() } : { tick: raw, off: 0 };
  },
  async finishRec() {
    const recs = this.recs, spt = this.spt(), L = songBars(S.p) * BAR; this.recs = []; this.recAudio = false;
    for (const i of this.inputs.values()) i.rec = null;
    let made = 0;
    for (const rec of recs) {
      const { tick, off } = this.takeStart(rec), dur = rec.len / rec.sr - off;
      if (dur < 0.15) continue;
      const buf = ctx.createBuffer(rec.stereo ? 2 : 1, rec.len, rec.sr);
      [rec.L, rec.R].forEach((chunks, c) => { if (c >= buf.numberOfChannels) return; const d = buf.getChannelData(c); let p = 0; for (const x of chunks) { d.set(x, p); p += x.length; } });
      const n = S.p.song.audio.filter(a => a.bus === rec.bus).length + 1;
      try {
        const id = await addBuffer(`${rec.name} take ${n}`, buf);
        const mk = (track, st, o, d, name, mute) => { if (track >= S.p.song.tracks) S.p.song.tracks = track + 1; S.p.song.audio.push({ id: uid(), sample: id, name, track, start: st / BAR, len: d / (BAR * spt), off: o, bus: rec.bus, gain: 1, bpm: S.p.bpm, stretch: true, mute }); made++; };
        if (rec.linear || !L || tick + dur / spt <= L + 1) mk(rec.track, tick, off, dur, `${rec.name} take ${n}`);
        else {
          // Loop recording: one clip per pass around the loop, stacked on following tracks; only the newest pass is left unmuted.
          const segs = []; for (let o = off, s = tick; dur - (o - off) > 0.15;) { const d = Math.min((L - s) * spt, dur - (o - off)); segs.push([s, o, d]); o += d; s = 0; }
          segs.forEach(([s, o, d], i) => mk(rec.track + i, s, o, d, `${rec.name} take ${n + i}`, i < segs.length - 1));
        }
      } catch { toast('Could not store the recording'); }
    }
    if (made) { commit(); toast(made > 1 ? `${made} takes recorded` : 'Take recorded'); }
  },
  // --- automation: lanes of points in song time that drive mixer and channel levels / pans -------
  // A plug-in parameter's name and range: from the loaded plug-in, or from what the slot remembered when a lane was made.
  plugParam(x, slot, key) {
    const q = x && x.info && x.info.params.find(z => 'v' + z.id === key); if (q) return { name: q.name, min: q.min, max: q.max };
    const m = slot && slot.meta && slot.meta[key]; return m ? { name: m[0], min: m[1], max: m[2] } : null;
  },
  // Remember a parameter's name and range in its slot, so its automation lane shows before the plug-in has loaded.
  rememberParam(target) {
    const tg = this.autoTarget(target); if (!tg || !tg.plug) return;
    (tg.obj.meta ||= {})[tg.key] = [tg.name, tg.min, tg.max];
  },
  autoTarget(target) {
    const [kind, id, key, prm] = target.split(':');
    if (kind === 'master') return { obj: S.p, key: 'masterPitch', kind, label: 'Master pitch', min: -12, max: 12 };   // master:pitch
    if (kind === 'plug') {   // plug:<channel>:v<parameter id>  (a plug-in instrument's parameter)
      const c = S.p.channels.find(z => z.id === id), s = c && this.graph && this.graph.strips.get(c.id), q = c && c.plug && this.plugParam(s && s.plug, c.plug, key);
      return q ? { obj: c.plug, key, kind, plug: true, chan: c, name: q.name, label: c.name + ' · ' + q.name, min: q.min, max: q.max } : null;
    }
    if (kind === 'fx') {   // fx:<bus>:<effect>:<parameter>
      const b = S.p.buses.find(z => z.id === id), f = b && b.fx.find(z => z.id === key);
      if (f && f.type === 'plugin') {   // a plug-in effect's parameter: v<parameter id>
        const n = this.graph && this.graph.buses.get(b.id), q = this.plugParam(n && n.fx[b.fx.indexOf(f)], f.p, prm);
        return q ? { obj: f.p, key: prm, kind, plug: true, bus: b, fx: f, name: q.name, label: b.name + ' · ' + (f.p.name || 'Plug-in') + ' ' + q.name, min: q.min, max: q.max } : null;
      }
      const d = f && FX[f.type].params.find(q => q.k === prm);
      return d && d.min != null ? { obj: f.p, key: prm, kind, bus: b, fx: f, label: b.name + ' · ' + FX[f.type].name + ' ' + d.label, min: d.min, max: d.max } : null;
    }
    const obj = kind === 'bus' ? S.p.buses.find(b => b.id === id) : S.p.channels.find(c => c.id === id);
    return obj ? { obj, key, kind, label: `${obj.name} · ${key === 'vol' ? 'level' : 'pan'}`, min: key === 'pan' ? -1 : 0, max: key === 'pan' ? 1 : 1.25 } : null;
  },
  autoAt(pts, t) {
    if (t <= pts[0].t) return pts[0].v;
    for (let i = 1; i < pts.length; i++) if (t < pts[i].t) { const a = pts[i - 1], b = pts[i]; return a.v + (b.v - a.v) * (t - a.t) / (b.t - a.t || 1); }
    return pts[pts.length - 1].v;
  },
  applyAuto(t) {
    if (S.mode !== 'song' || !S.p.song.auto.length) return;
    let changed = false;
    for (const l of S.p.song.auto) {
      if (!l.pts.length || this.writing.has(l.target)) continue;
      const g = this.autoTarget(l.target); if (!g) continue;
      const v = this.autoAt(l.pts, t);
      if (!(Math.abs(g.obj[g.key] - v) <= 1e-4)) { g.obj[g.key] = v; changed = true; }   // also when the value was never set (plug-in parameters)
    }
    if (changed) { this.graph.sync(); this.autoDirty = true; }
  },
  // Called by faders and knobs as they move. While recording automation in SONG mode the moves are captured.
  autoWrite(target, v) {
    if (!(this.playing && S.rec && prefs.recWhat.auto && S.mode === 'song')) return;
    let w = this.writing.get(target); if (!w) this.writing.set(target, (w = []));
    const t = Math.round(this.pos()), last = w[w.length - 1];
    if (last && Math.abs(t - last.t) < 6) last.v = v; else w.push({ t, v });
  },
  flushAuto() {
    if (!this.writing.size) return;
    for (const [target, w] of this.writing) {
      let l = S.p.song.auto.find(x => x.target === target); if (!l) S.p.song.auto.push((l = { id: uid(), target, pts: [] }));
      const a = Math.min(...w.map(p => p.t)), b = Math.max(...w.map(p => p.t));
      l.pts = [...l.pts.filter(p => p.t < a || p.t > b), ...w].sort((x, y) => x.t - y.t);
      this.rememberParam(target);
    }
    this.writing.clear(); commit(); toast('Automation recorded');
  },
  previewBuffer(buffer, gain = 0.8) {
    this.resume(); if (this.pv) { try { this.pv.stop(); } catch { /* ended */ } }
    const n = this.pv = ctx.createBufferSource(), g = ctx.createGain(); g.gain.value = gain; n.buffer = buffer; n.connect(g); g.connect(ctx.destination); n.start();
  },
  click(t, accent) {
    const o = ctx.createOscillator(), g = ctx.createGain(); t = Math.max(t, ctx.currentTime);
    o.frequency.value = accent ? 1560 : 1040; g.gain.setValueAtTime(accent ? 0.35 : 0.22, t); g.gain.exponentialRampToValueAtTime(0.001, t + 0.05);
    o.connect(g); g.connect(ctx.destination); o.start(t); o.stop(t + 0.06);
  },
  preview(c, note, vel = 0.8, dur = 0.3) { this.resume(); return this.graph.trigger(c, note, vel, ctx.currentTime, dur, { bend: this.bend }); },
  previewSample(id) {
    this.resume(); if (this.pv) { try { this.pv.stop(); } catch { /* ended */ } }
    const s = samples.get(id); if (!s) return;
    const n = this.pv = ctx.createBufferSource(), g = ctx.createGain(); g.gain.value = 0.8; n.buffer = s.buffer; n.connect(g); g.connect(ctx.destination); n.start();
  },
  // Live input: on-screen keys, typing keyboard, MIDI devices and the voice-to-MIDI tracker all come through here.
  noteOn(note, vel = 0.8, c = chan()) {
    if (!c || note < 0 || note > 127) return;
    if (this.waiting) this.play(this.waiting.from, true);   // "wait for input": the first note starts the recording
    this.resume();
    const key = c.id + ':' + note;
    this.noteOff(note, c, true);
    this.held.set(key, this.graph.trigger(c, note, vel, ctx.currentTime, null, { bend: this.bend, wheel: this.wheel }));
    if (S.rec && prefs.recWhat.midi && this.playing && S.mode !== 'song') this.recStart.set(key, { t: this.pos() - (ctx.outputLatency || 0) / this.spt(), vel });
    bus.emit('key', { note, on: true });
  },
  noteOff(note, c = chan(), force) {
    if (!c) return;
    const key = c.id + ':' + note;
    if (this.sustain && !force) { this.sus.set(key, [note, c]); return; }
    const v = this.held.get(key);
    if (v) { v.release(ctx.currentTime); this.held.delete(key); }
    const r = this.recStart.get(key);
    if (r) {
      this.recStart.delete(key);
      const p = pat(), L = p.bars * BAR, q = prefs.recQuant;
      let t = ((r.t % L) + L) % L, d = this.pos() - t; if (d <= 0) d += L;
      if (q) t = (Math.round(t / q) * q) % L;
      notesOf(p, c.id).push({ id: uid(), t: Math.round(t), d: Math.max(q || 6, Math.round(Math.min(d, L - t))), n: note, v: r.vel });
      this.recDirty = true; bus.emit('notes');
    }
    bus.emit('key', { note, on: false });
  },
  setSustain(on) { const p = this.plugOf(); if (p) p.midi([0xB0, 64, on ? 127 : 0]); this.sustain = on; if (!on) { for (const [n, c] of this.sus.values()) this.noteOff(n, c, true); this.sus.clear(); } },
  // Controllers also go to the selected channel when it is a plug-in instrument, as MIDI (bend, CC 1, CC 64).
  plugOf(c = chan()) { const s = c && c.type === 'plugin' && this.graph && this.graph.strips.get(c.id); return s && s.plug; },
  setBend(x) { const p = this.plugOf(), v = Math.max(0, Math.min(16383, Math.round((x + 1) * 8192))); if (p) p.midi([0xE0, v & 127, v >> 7]);
    const c = chan(); this.bend = x * (c && c.synth ? c.synth.bendRange : 2) * 100; for (const v of this.held.values()) if (v && v.bend) v.bend.value = v.bendBase + this.bend; },
  setWheel(x) { const p = this.plugOf(); if (p) p.midi([0xB0, 1, Math.round(x * 127)]); this.wheel = x; for (const v of this.held.values()) if (v && v.wheel) v.wheel.value = x; },
  eachVoice(id, fn) { (this.graph.voices.get(id) || []).forEach(fn); },
  wave(id) { const n = this.graph && this.graph.buses.get(id); if (!n || !n.an) return null; n.an.getFloatTimeDomainData(n.buf); return n.buf; },
  level(id) {
    const n = this.graph && this.graph.buses.get(id); if (!n || !n.an) return 0;
    n.an.getFloatTimeDomainData(n.buf); let m = 0;
    for (let i = 0; i < 512; i++) { const a = Math.abs(n.buf[i]); if (a > m) m = a; }
    return m;
  },
  // Bounce the current pattern or the whole song through an identical offline graph.
  // Plug-ins live in other processes and cannot run inside an offline graph, so their audio is rendered through their
  // hosts first: instruments from their notes, then effects layer by layer (an effect needs everything before it done),
  // each layer taking one extra offline pass that records what arrives at those effects.
  async render(mode, loops = 1) {
    const P = S.p, sr = prefs.exportRate || 44100, spt = 60 / P.bpm / PPQ, L = (mode === 'song' ? songBars(P) * BAR : pat().bars * BAR) * loops;
    if (!L) throw new Error('Nothing to render: the song is empty');
    const frames = Math.ceil((L * spt + 2.5) * sr), evs = collect(P, mode, pat(), 0, L).sort((x, y) => x.tick + x.sw - y.tick - y.sw);
    const toBuffer = ([l, r]) => { const b = new AudioBuffer({ numberOfChannels: 2, length: frames, sampleRate: sr }); b.copyToChannel(l, 0); b.copyToChannel(r, 1); return b; };
    const failed = []; offlineAudio.clear();
    // Automation of a plug-in's parameters, as timed parameter changes for its render (every 512 frames where it moves).
    const lanesFor = prefix => (mode === 'song' ? P.song.auto.filter(l => l.target.startsWith(prefix) && l.target.slice(prefix.length)[0] === 'v' && l.pts.length) : []);
    const paramEvents = prefix => lanesFor(prefix).flatMap(l => {
      const id = +l.target.slice(prefix.length + 1), out = []; let last;
      for (let f = 0; f < frames; f += 512) { const v = this.autoAt(l.pts, f / sr / spt); if (v !== last) { out.push({ frame: f, id, value: v }); last = v; } }
      return out;
    });
    const merge = (a, b) => [...a, ...b].sort((x, y) => x.frame - y.frame || (x.id != null) - (y.id != null));
    try {
      for (const c of P.channels) if (c.type === 'plugin' && c.plug && c.plug.path) {
        const cc = {}, ccs = (f, n, val) => { const k = Math.round(clamp(64 + (val || 0) * 63, 0, 127)); if (cc[n] !== k && (k !== 64 || cc[n] != null)) { cc[n] = k; return [{ frame: f, data: [0xB0, n, k] }]; } return []; };
        const notes = evs.filter(e => e.c === c).flatMap(e => { const t = (e.tick + e.sw) * spt, f = Math.round(t * sr); return [...ccs(f, 74, e.cut), ...ccs(f, 71, e.res), { frame: f, on: true, key: e.n, vel: e.v }, { frame: Math.round((t + e.d * spt) * sr), on: false, key: e.n, vel: 0 }]; }).sort((x, y) => x.frame - y.frame || !!x.on - !!y.on);
        if (!notes.length) continue;
        try { offlineAudio.set(c.plug, toBuffer(await renderOffline(c.plug, sr, frames, null, merge(notes, paramEvents('plug:' + c.id + ':'))))); } catch (e) { failed.push(c.name + ' (' + e.message + ')'); }
      }
      let todo = P.buses.flatMap(b => b.fx.map((f, i) => ({ b, i, f })).filter(z => z.f.type === 'plugin' && z.f.on && z.f.p.path));
      const before = (x, y) => (x.b === y.b ? x.i < y.i : y.b.kind === 'master' || reaches(P, x.b.id, y.b.id));   // x's output feeds y
      while (todo.length) {
        let ready = todo.filter(y => !todo.some(x => x !== y && before(x, y))).slice(0, 15); if (!ready.length) ready = [todo[0]];
        const pass = await this.renderPass(mode, L, evs, ready);
        for (const [k, z] of ready.entries()) {
          try { offlineAudio.set(z.f.p, toBuffer(await renderOffline(z.f.p, sr, frames, [pass.getChannelData(2 + k * 2), pass.getChannelData(3 + k * 2)], paramEvents('fx:' + z.b.id + ':' + z.f.id + ':')))); } catch (e) { failed.push((z.f.p.name || 'plug-in') + ' (' + e.message + ')'); }
        }
        todo = todo.filter(z => !ready.includes(z));
      }
      const out = await this.renderPass(mode, L, evs, []);
      if (failed.length) out.note = 'left out: ' + failed.join(', ');
      return out;
    } finally { offlineAudio.clear(); }
  },
  // One offline render. `taps` are plug-in effect slots whose incoming audio is recorded on extra channel pairs after the mix.
  async renderPass(mode, L, evs, taps) {
    const P = S.p, sr = prefs.exportRate || 44100, spt = 60 / P.bpm / PPQ;
    const octx = new OfflineAudioContext(2 + taps.length * 2, Math.ceil((L * spt + 2.5) * sr), sr); await loadFxWorklets(octx);
    if (taps.length) { octx.destination.channelCount = 2 + taps.length * 2; octx.destination.channelCountMode = 'explicit'; octx.destination.channelInterpretation = 'discrete'; }
    const g = new Graph(octx, false);
    if (taps.length) {
      const mg = octx.createChannelMerger(2 + taps.length * 2); mg.connect(octx.destination);
      taps.forEach((z, k) => { const sp = octx.createChannelSplitter(2); g.buses.get(z.b.id).fx[z.i].inp.connect(sp); sp.connect(mg, 0, 2 + k * 2); sp.connect(mg, 1, 3 + k * 2); });
    }
    for (const e of evs) g.trigger(e.c, e.n, e.v, (e.tick + e.sw) * spt, e.d * spt, noteOpts(e));
    if (mode === 'song') for (const l of P.song.auto) {
      const tg = this.autoTarget(l.target); if (!tg || !l.pts.length || tg.kind === 'fx' || tg.kind === 'plug' || tg.obj.mute) continue;
      const n = tg.kind === 'bus' ? g.buses.get(tg.obj.id) : g.strips.get(tg.obj.id); if (!n) continue;
      const prm = tg.key === 'pan' ? n.pan.pan : tg.kind === 'bus' ? n.fader.gain : n.g.gain;
      prm.setValueAtTime(l.pts[0].v, 0); for (const p of l.pts) prm.linearRampToValueAtTime(p.v, Math.max(0.001, p.t * spt));
    }
    if (mode === 'song') for (const a of P.song.audio) if (!a.mute) g.playClip(a, this.clipBuf(a), Math.round(a.start * BAR) * spt, (a.off || 0) * this.ratio(a), this.clipDur(a));
    // Effect parameters have no single audio parameter to draw a ramp on, so the offline render pauses every 50 ms and sets them.
    const fxLanes = mode === 'song' ? P.song.auto.map(l => ({ l, tg: this.autoTarget(l.target) })).filter(z => z.tg && z.tg.kind === 'fx' && z.l.pts.length) : [];
    if (fxLanes.length && octx.suspend) for (let t = 0.05; t < octx.length / sr - 0.05; t += 0.05) octx.suspend(t).then(() => {
      for (const { l, tg } of fxLanes) { const n = g.buses.get(tg.bus.id), x = n && n.fx[tg.bus.fx.indexOf(tg.fx)]; if (x) x.set(tg.key, this.autoAt(l.pts, t / spt)); }
      octx.resume();
    });
    return octx.startRendering();
  },
};
bus.on('project', () => engine.refresh());
bus.on('autowrite', ({ target, v }) => engine.autoWrite(target, v));   // parameter changes made in a plug-in's own window