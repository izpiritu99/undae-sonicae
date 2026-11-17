import { touch, pinch } from '../touch.js';
// Song arranger: pattern clips and recorded audio clips on tracks along a bar timeline.
import { h, sel, btn, bus, ask, menu, menuAt, clamp, uid, toast, BAR, BEAT, PPQ } from '../util.js';
import { S, pat, commit, newPattern, songBars, COLORS, prefs, savePrefs, busLabel } from '../state.js';
import { engine } from '../audio/engine.js';
import { samples, dropped } from '../samples.js';
import { FX } from '../audio/fx.js';
import { actionFor } from '../keys.js';

// Snap values are fractions of a bar; note values follow the time signature (a 1/4 note is PPQ ticks).
const SNAPS = () => [[1, 'Bar'], [0.5, '1/2 bar'], [PPQ / BAR, '1/4'], [PPQ / 2 / BAR, '1/8'], [PPQ / 3 / BAR, '1/8T'], [PPQ / 4 / BAR, '1/16'], [PPQ / 6 / BAR, '1/16T'], [PPQ / 8 / BAR, '1/32'], [PPQ / 16 / BAR, '1/64'], [0, 'Off']];
const CLIP_COLORS = [['#ff7a59', 'Coral'], ['#ffb84d', 'Amber'], ['#e6d15a', 'Yellow'], ['#7bd88f', 'Green'], ['#45d0c4', 'Teal'], ['#5aa9ff', 'Blue'], ['#9d8cff', 'Violet'], ['#f277c6', 'Pink'], ['#e5484d', 'Red'], ['#a3a3a3', 'Grey'], ['#f0ebe2', 'Paper'], ['#6b5b4b', 'Brown']];
const TOOLS = [['draw', 'Draw (P): click to place the picked pattern, drag to move, right edge to resize, right-click to delete'], ['paint', 'Paint (B): drag along a track to paint repeated clips'],
  ['erase', 'Delete (D): click or drag over clips to delete them'], ['mute', 'Mute (T): click or drag over clips to mute / unmute them'], ['slip', 'Slip (S): drag inside a clip to move its content, not the clip'],
  ['slice', 'Slice (C): click a clip to cut it in two at that point'], ['select', 'Select (E): drag a box to select; right-click a clip for its menu'], ['zoom', 'Zoom (Z): click to zoom in, right-click to zoom out'],
  ['play', 'Play (Y): click to start playing the song from there']];

export default function Song() {
  const pal = h('div', { class: 'pal' }), ruler = h('div', { class: 'sruler' }), lanes = h('div', { class: 'lanes' }), head = h('div', { class: 'phead' }), mark = h('div', { class: 'smark' }), box = h('div', { class: 'marquee' });
  const autos = h('div', { class: 'autos' }), selBox = h('div', { class: 'sselbox' }), inner = h('div', { class: 'sinner' }, ruler, lanes, autos, selBox, head, mark), scroll = h('div', { class: 'sscroll', tabIndex: 0 }, inner), bar = h('div', { class: 'vbar' });
  const el = h('div', { class: 'view song' }, bar, h('div', { class: 'sbody' }, pal, scroll));
  let bw = 56, bars = 32, TH = prefs.songTH || 46; const AH = 46, NS = 'http://www.w3.org/2000/svg', picked = new Set(), live = new Map();
  const colorOf = id => { const p = S.p.patterns.find(q => q.id === id); return (p && p.color) || COLORS[Math.max(0, S.p.patterns.indexOf(p)) % COLORS.length]; };
  const clipColor = c => c.color || (isAudio(c) ? 'var(--accent2)' : colorOf(c.pat));
  let tool = 'draw', clipboard = [];
  const all = () => [...S.p.song.clips, ...S.p.song.audio];
  const isAudio = c => 'sample' in c;
  // Snap a bar position to the chosen grid; holding Alt while dragging frees it.
  const grid = e => (e && e.altKey ? 0 : prefs.songSnap);
  const snap = (v, e, floor) => { const s = grid(e); return s ? (floor ? Math.floor(v / s + 1e-6) : Math.round(v / s)) * s : Math.round(v * BAR) / BAR; };
  const barSec = () => BAR * 60 / S.p.bpm / PPQ;

  function preview(p, w, hh) {
    const cv = h('canvas', { width: Math.max(1, Math.round(w)), height: hh }), g = cv.getContext('2d'), notes = Object.values(p.notes).flat();
    if (!notes.length) return cv;
    const lo = Math.min(...notes.map(n => n.n)), hi = Math.max(...notes.map(n => n.n)), span = Math.max(1, hi - lo), pl = p.bars * BAR, k = bw / BAR;
    g.fillStyle = 'rgba(0,0,0,.55)';
    for (let x0 = 0; x0 < w; x0 += pl * k) for (const n of notes) if (n.t < pl) g.fillRect(x0 + n.t * k, 2 + (1 - (n.n - lo) / span) * (hh - 6), Math.max(1.5, n.d * k - 0.5), 2);
    return cv;
  }
  function waveform(c, w, hh) {
    const cv = h('canvas', { width: clamp(Math.round(w), 1, 4000), height: hh }), g = cv.getContext('2d'), smp = samples.get(c.sample);
    if (!smp) return cv;
    const d = smp.buffer.getChannelData(0), sr = smp.buffer.sampleRate, a = Math.floor((c.off || 0) * sr), n = Math.min(d.length - a, Math.floor(c.len * barSec() / engine.ratio(c) * sr)), W = cv.width, per = n / W, stride = Math.max(1, Math.floor(per / 40));
    g.fillStyle = 'rgba(0,0,0,.6)';
    for (let x = 0; x < W; x++) { let m = 0; for (let i = a + Math.floor(x * per), e = Math.min(d.length, i + per); i < e; i += stride) { const v = Math.abs(d[i]); if (v > m) m = v; } const y = Math.max(1, m * hh); g.fillRect(x, (hh - y) / 2, 1, y); }
    return cv;
  }
  const place = (node, c) => { node.style.left = c.start * bw + 'px'; node.style.top = c.track * TH + 2 + 'px'; node.style.width = Math.max(4, c.len * bw - 2) + 'px'; };
  function clipNode(c) {
    const P = S.p, p = !isAudio(c) && P.patterns.find(x => x.id === c.pat);
    if (!isAudio(c) && !p) return null;
    const node = h('div', { class: 'clip' + (isAudio(c) ? ' audio' : '') + (c.mute ? ' muted' : '') + (picked.has(c.id) ? ' picked' : ''), 'data-id': c.id, style: `height:${TH - 4}px;--c:${clipColor(c)}` },
      h('b', null, (c.group ? '⛓ ' : '') + (isAudio(c) ? `${c.name || 'Audio'} → ${busLabel(P, c.bus)}${Math.abs(engine.ratio(c) - 1) > 0.002 ? ' · ' + Math.round(100 / engine.ratio(c)) + '% speed' : ''}` : p.name + (c.off ? ' ↔' : ''))), prefs.songPreview === false || TH < 30 ? '' : isAudio(c) ? waveform(c, c.len * bw, TH - 20) : preview(p, c.len * bw, TH - 20));
    place(node, c);
    return node;
  }
  const TOOL_ICON = { draw: '✎', paint: '🖌', erase: '⌫', mute: '🔇', slip: '⇔', slice: '✂', select: '⬚', zoom: '🔍', play: '▶' };
  function setTool(id) { tool = id; scroll.dataset.tool = id; toolbar(); }
  // --- arrangements: P.arrs = [{ id, name, song? }]; the current one lives in P.song, the others keep their own copy.
  function arrangements() { const P = S.p; if (!P.arrs || !P.arrs.length) { P.arrs = [{ id: uid(), name: 'Arrangement' }]; P.arrCur = P.arrs[0].id; } return P.arrs; }
  function switchArr(id) {
    const P = S.p, cur = P.arrs.find(a => a.id === P.arrCur), next = P.arrs.find(a => a.id === id); if (!next || next === cur) return;
    if (engine.playing) engine.stop();
    if (cur) cur.song = P.song; P.song = next.song || { tracks: P.song.tracks, clips: [], audio: [], auto: [], markers: [] }; delete next.song; P.arrCur = id; picked.clear(); commit();
  }
  function arrMenu() {
    const P = S.p, cur = P.arrs.find(a => a.id === P.arrCur);
    return [{ head: 'Arrangement · ' + (cur ? cur.name : '') },
      { label: 'Add one…', fn: async () => { const v = await ask('Arrangement name', 'Arrangement ' + (P.arrs.length + 1)); if (!v) return; const a = { id: uid(), name: v }; P.arrs.push(a); switchArr(a.id); } },
      { label: 'Clone…', fn: async () => { const v = await ask('Name of the copy', cur.name + ' copy'); if (!v) return; const a = { id: uid(), name: v, song: JSON.parse(JSON.stringify(P.song)) }; a.song.clips.forEach(c => { c.id = uid(); }); a.song.audio.forEach(c => { c.id = uid(); }); P.arrs.push(a); switchArr(a.id); } },
      { label: 'Rename…', fn: async () => { const v = await ask('Arrangement name', cur.name); if (v) { cur.name = v; commit(); } } },
      { label: 'Delete', disabled: P.arrs.length < 2, fn: () => { const other = P.arrs.find(a => a !== cur); switchArr(other.id); P.arrs = P.arrs.filter(a => a !== cur); commit(); } }];
  }
  function toolbar() {
    const P = S.p, arrs = arrangements();
    bar.replaceChildren(
      h('button', { class: 'btn sm', title: 'Song menu: edit, tools, snap, select, group, zoom, time markers, arrangements', onclick: e => menuAt(e.currentTarget, songMenu()) }, '☰'),
      h('div', { class: 'seg tools' }, TOOLS.map(([id, t]) => h('button', { class: 'tb' + (tool === id ? ' on' : ''), title: t, onclick: () => setTool(id) }, TOOL_ICON[id]))),
      h('label', { title: 'Grid that clips snap to when placed, moved or resized. Hold Alt while dragging to move freely.' }, 'Snap'), sel(SNAPS(), prefs.songSnap, v => { prefs.songSnap = +v; savePrefs(); build(); }),
      h('label', { title: 'Arrangements: separate versions of the song (intro-less radio edit, live set…), each with its own clips' }, 'Arrangement'),
      sel(arrs.map(a => [a.id, a.name]), P.arrCur || arrs[0].id, v => switchArr(v)), btn('▾', e => menuAt(e.currentTarget, arrMenu()), 'sm', 'Add, clone, rename or delete arrangements'),
      h('div', { class: 'seg touchonly', title: 'Editing without a keyboard' }, btn('Copy', copy, '', 'Copy the selected clips'), btn('Paste', paste, '', 'Paste at the play marker'),
        btn('Dup', duplicate, '', 'Duplicate the selected clips'), btn('Del', () => { if (picked.size) drop(new Set(picked)); }, '', 'Delete the selected clips')),
      h('span', { class: 'grow' }),
      h('button', { class: 'btn sm', title: 'Add an automation lane: draw how a level or pan changes over the song', onclick: e => {
        const at = e.currentTarget, g = engine.graph;
        const plugins = [...S.p.buses.flatMap(b => b.fx.map((f, i) => f.type === 'plugin' && f.p.path && { label: busLabel(S.p, b.id) + ' · ' + (f.p.name || 'Plug-in'), slot: f.p, prefix: 'fx:' + b.id + ':' + f.id + ':', x: g && g.buses.get(b.id) && g.buses.get(b.id).fx[i] })),
          ...S.p.channels.map(c => c.type === 'plugin' && c.plug && c.plug.path && { label: c.name, slot: c.plug, prefix: 'plug:' + c.id + ':', x: g && g.strips.get(c.id) && g.strips.get(c.id).plug })].filter(Boolean);
        const P = S.p, add = (target, v) => { if (!P.song.auto.some(l => l.target === target)) P.song.auto.push({ id: uid(), target, pts: [{ t: 0, v }] }); commit(); };
        menuAt(at, [{ head: 'Mixer buses' }, ...P.buses.flatMap(b => [{ label: `${busLabel(P, b.id)} · ${b.name} level`, fn: () => add(`bus:${b.id}:vol`, b.vol) }, { label: `${busLabel(P, b.id)} · ${b.name} pan`, fn: () => add(`bus:${b.id}:pan`, b.pan) }]),
          { head: 'Effects' }, ...P.buses.flatMap(b => b.fx.flatMap(f => FX[f.type].params.filter(q => q.min != null).map(q => ({ label: busLabel(P, b.id) + ' · ' + FX[f.type].name + ' ' + q.label, fn: () => add('fx:' + b.id + ':' + f.id + ':' + q.k, f.p[q.k]) })))),
          { head: 'Channels' }, ...P.channels.flatMap(c => [{ label: c.name + ' level', fn: () => add(`ch:${c.id}:vol`, c.vol) }, { label: c.name + ' pan', fn: () => add(`ch:${c.id}:pan`, c.pan) }]),
          ...plugins.length ? [{ head: 'Plug-in parameters' }, ...plugins.map(z => ({ label: z.label + ' …', fn: () => paramMenu(z) }))] : []]);
        // Plug-ins can have hundreds of parameters, so each gets its own menu, listing what the loaded plug-in reports.
        function paramMenu(z) {
          const ps = z.x && z.x.info ? z.x.info.params.filter(q => !q.hidden && !q.readonly) : [];
          if (!ps.length) return toast('The plug-in is not loaded yet');
          menuAt(at, [{ head: z.label }, ...ps.map(q => ({ label: q.name || 'Parameter ' + q.id, fn: () => { const t = z.prefix + 'v' + q.id; add(t, z.slot['v' + q.id] ?? q.value); engine.rememberParam(t); commit(); } }))]);
        }
      } }, '+ Automation'),
      btn('+ Track', () => { S.p.song.tracks++; commit(); }, 'sm'), btn('−', () => zoom(0.75), 'sm', 'Zoom out (Ctrl+wheel)'), btn('+', () => zoom(1.33), 'sm', 'Zoom in (Ctrl+wheel)'));
  }
  // --- editing ------------------------------------------------------------
  const listOf = c => (isAudio(c) ? S.p.song.audio : S.p.song.clips);
  const chosen = () => all().filter(c => picked.has(c.id));
  const grid1 = () => prefs.songSnap || 1 / 16;
  const patOf = c => S.p.patterns.find(q => q.id === c.pat);
  const withGroups = ids => { const gs = new Set(all().filter(c => ids.has(c.id) && c.group).map(c => c.group)); if (gs.size) for (const c of all()) if (gs.has(c.group)) ids.add(c.id); return ids; };
  function duplicate() {
    const group = chosen(); if (!group.length) return;
    const a = Math.min(...group.map(c => c.start)), b = Math.max(...group.map(c => c.start + c.len)); picked.clear();
    for (const c of group) { const z = { ...c, id: uid(), start: c.start + (b - a) }; listOf(c).push(z); picked.add(z.id); }
    commit();
  }
  const shift = d => { const g = chosen(); if (!g.length) return; const m = Math.min(...g.map(c => c.start)); d = Math.max(-m, d); g.forEach(c => { c.start += d; }); commit(); };
  const setMute = v => { chosen().forEach(c => { c.mute = v; }); commit(); };
  const colour = (cs, col) => { cs.forEach(c => { if (col) c.color = col; else delete c.color; }); commit(); };
  function split(c, at) {   // cut a clip in two at a bar position (no commit); returns the right half
    if (at <= c.start + 1e-6 || at >= c.start + c.len - 1e-6) return null;
    const left = at - c.start, z = { ...c, id: uid(), start: at, len: c.len - left };
    if (isAudio(c)) z.off = (c.off || 0) + left * barSec() / engine.ratio(c); else { const pl = patOf(c).bars; z.off = (((c.off || 0) + left) % pl + pl) % pl; }
    c.len = left; listOf(c).push(z); return z;
  }
  const sliceClip = (c, at) => { if (split(c, at)) commit(); };
  // The range for Insert / Delete space: the ruler selection, or one snap step at the play marker.
  const spaceRange = () => (S.songSel && S.songSel.b > S.songSel.a ? [S.songSel.a, S.songSel.b] : [S.songStart, S.songStart + (prefs.songSnap || 1)]);
  function insertSpace() {
    const [a, b] = spaceRange(), d = b - a;
    for (const c of all()) split(c, a);                                          // clips across the point are cut there first
    for (const c of all()) if (c.start >= a - 1e-6) c.start += d;
    S.p.song.auto.forEach(l => l.pts.forEach(p => { if (p.t >= a * BAR) p.t += d * BAR; }));
    (S.p.song.markers || []).forEach(m => { if (m.t >= a) m.t += d; });
    commit(); toast(`Inserted ${+(d).toFixed(3)} bar${d === 1 ? '' : 's'} of space`);
  }
  function deleteSpace() {
    const [a, b] = spaceRange(), d = b - a, song = S.p.song;
    const keep = c => {
      const e = c.start + c.len;
      if (c.start >= a && e <= b) return false;                                  // wholly inside: gone
      if (c.start < a && e > a) { c.len = (a - c.start) + Math.max(0, e - b); return true; }   // runs into the range: shorter
      if (c.start >= a && c.start < b) { const cut = b - c.start; c.len = e - b; c.start = a; if (isAudio(c)) c.off = (c.off || 0) + cut * barSec() / engine.ratio(c); else { const pl = patOf(c).bars; c.off = (((c.off || 0) + cut) % pl + pl) % pl; } return true; }
      if (c.start >= b) c.start -= d; return true;
    };
    song.clips = song.clips.filter(keep); song.audio = song.audio.filter(keep);
    song.auto.forEach(l => { l.pts = l.pts.filter(p => p.t < a * BAR || p.t >= b * BAR); l.pts.forEach(p => { if (p.t >= b * BAR) p.t -= d * BAR; }); if (!l.pts.length) l.pts = [{ t: 0, v: 0 }]; });
    song.markers = (song.markers || []).filter(m => m.t < a || m.t >= b); song.markers.forEach(m => { if (m.t >= b) m.t -= d; });
    picked.clear(); commit(); toast(`Deleted ${+(d).toFixed(3)} bar${d === 1 ? '' : 's'} of space`);
  }
  const quantStarts = () => { const g = chosen().length ? chosen() : all(), q = prefs.songSnap || 1; g.forEach(c => { c.start = Math.max(0, Math.round(c.start / q) * q); }); commit(); toast('Clip starts quantized'); };
  function makeUnique(cs) {
    for (const c of cs.filter(x => !isAudio(x))) { const p = patOf(c); if (!p) continue; const n = JSON.parse(JSON.stringify(p)); n.id = uid(); n.name = p.name + ' (unique)'; for (const k in n.notes) n.notes[k].forEach(z => { z.id = uid(); }); S.p.patterns.push(n); c.pat = n.id; }
    commit();
  }
  const copy = () => { const g = chosen(); if (!g.length) return; const a = Math.min(...g.map(c => c.start)); clipboard = g.map(c => ({ ...c, start: c.start - a })); toast(`Copied ${g.length} clip${g.length === 1 ? '' : 's'}`); };
  const paste = () => { if (!clipboard.length) return; picked.clear(); for (const c of clipboard) { const z = { ...c, id: uid(), start: c.start + S.songStart }; listOf(z).push(z); picked.add(z.id); } commit(); };
  const group = () => { const g = chosen(); if (g.length < 2) return toast('Select two or more clips to group'); const id = uid(); g.forEach(c => { c.group = id; }); commit(); toast('Grouped: selecting one selects all of them'); };
  const ungroup = () => { chosen().forEach(c => { delete c.group; }); commit(); };
  const selectWhere = f => { picked.clear(); all().filter(f).forEach(c => picked.add(c.id)); paint(); };
  const bySource = () => { const g = chosen(); if (!g.length) return; const pats = new Set(g.map(c => c.pat).filter(Boolean)), smps = new Set(g.map(c => c.sample).filter(Boolean)); selectWhere(c => pats.has(c.pat) || smps.has(c.sample)); };
  function addMarker() { (S.p.song.markers ||= []).push({ id: uid(), t: S.songStart, name: 'Marker ' + (S.p.song.markers.length + 1) }); commit(); }
  function playFrom(barPos) {
    if (S.mode !== 'song') { if (engine.playing) engine.stop(); S.mode = 'song'; bus.emit('sel'); }
    if (engine.playing) engine.stop();
    S.songStart = barPos; engine.play(Math.round(barPos * BAR));
  }
  function zoomFit(range) {
    const [a, b] = range || [0, Math.max(4, songBars(S.p))];
    bw = clamp((scroll.clientWidth - 20) / Math.max(1, b - a), 10, 480); build(); scroll.scrollLeft = a * bw;
  }
  const colourItems = cs => [...CLIP_COLORS.map(([c, n]) => ({ label: '● ' + n, fn: () => colour(cs, c) })), { label: 'Back to the pattern colour', fn: () => colour(cs, null) },
    { label: 'Pick any colour…', fn: () => { const inp = h('input', { type: 'color', value: '#5aa9ff' }); inp.addEventListener('change', () => colour(cs, inp.value)); inp.click(); } }];
  // View: track height and which parts of the Playlist show.
  const HEIGHTS = [[26, 'Compact'], [34, 'Small'], [46, 'Normal'], [64, 'Large'], [90, 'Extra large']];
  const setTH = v => { prefs.songTH = clamp(Math.round(v), 22, 140); savePrefs(); TH = prefs.songTH; build(); };
  const flip = k => { prefs[k] = prefs[k] === false; savePrefs(); build(); };
  const viewMenu = () => [{ head: 'Track height' }, ...HEIGHTS.map(([v, l]) => ({ label: l, hint: v + ' px', on: TH === v, fn: () => setTH(v) })), { label: 'Taller', hint: 'Alt+wheel', fn: () => setTH(TH * 1.2) }, { label: 'Shorter', hint: 'Alt+wheel', fn: () => setTH(TH / 1.2) }, '-',
    { label: 'Picker panel', on: prefs.songPicker !== false, fn: () => flip('songPicker') }, { label: 'Clip contents (notes and waveforms)', on: prefs.songPreview !== false, fn: () => flip('songPreview') },
    { label: 'Automation lanes', on: prefs.songAutos !== false, fn: () => flip('songAutos') }, { label: 'Time markers', on: prefs.songMarkers !== false, fn: () => flip('songMarkers') }];
  // Picker panel: the patterns and audio clips you can place. Click one, then draw or paint with it, or drag it onto a track.
  // S.songSrc = { audio: sample id } while an audio clip is picked; patterns use S.pat as before.
  const audioSources = () => { const m = new Map(); for (const a of S.p.song.audio) if (!m.has(a.sample)) m.set(a.sample, a); return [...m.values()]; };
  const pickerMenu = () => {
    const o = prefs.picker || (prefs.picker = {}), set = (k, v) => { o[k] = v; savePrefs(); build(); };
    return [{ head: 'Picker' }, { label: 'Show patterns', on: o.pats !== false, fn: () => set('pats', o.pats === false) }, { label: 'Show audio clips', on: o.audio !== false, fn: () => set('audio', o.audio === false) },
      { label: 'Hide what the song does not use', on: !!o.used, fn: () => set('used', !o.used) }, '-',
      { head: 'Sort' }, ...[['order', 'In order'], ['name', 'By name'], ['colour', 'By colour'], ['len', 'By length']].map(([k, l]) => ({ label: l, on: (o.sort || 'order') === k, fn: () => set('sort', k) })), '-',
      { label: 'Hide the picker panel', fn: () => flip('songPicker') }];
  };
  const audioClipAt = (src, track, start) => { const z = { ...JSON.parse(JSON.stringify(src)), id: uid(), track, start, off: 0 }; delete z.group; delete z.mute; S.p.song.audio.push(z); return z; };
  function songMenu() {
    const n = picked.size, g = chosen();
    return [
      { label: 'View', sub: viewMenu() },
      { label: 'Edit', sub: [
        { label: 'Cut', hint: 'Ctrl+X', disabled: !n, fn: () => { copy(); drop(new Set(picked)); } }, { label: 'Copy', hint: 'Ctrl+C', disabled: !n, fn: copy }, { label: 'Paste at the play marker', hint: 'Ctrl+V', disabled: !clipboard.length, fn: paste },
        { label: 'Duplicate', hint: 'Ctrl+B', disabled: !n, fn: duplicate }, { label: 'Delete', hint: 'Del', disabled: !n, fn: () => drop(new Set(picked)) }, '-',
        { label: 'Shift left', hint: 'one snap step', disabled: !n, fn: () => shift(-grid1()) }, { label: 'Shift right', disabled: !n, fn: () => shift(grid1()) }, '-',
        { label: 'Mute', disabled: !n, fn: () => setMute(true) }, { label: 'Unmute', disabled: !n, fn: () => setMute(false) }, { label: 'Colour', disabled: !n, sub: colourItems(g) }, { label: 'Make unique (own copy of the pattern)', disabled: !g.some(c => !isAudio(c)), fn: () => makeUnique(g) }, '-',
        { label: 'Insert space', hint: 'Ctrl+Ins', fn: insertSpace }, { label: 'Delete space', hint: 'Ctrl+Del', fn: deleteSpace }] },
      { label: 'Tools', sub: [{ label: 'Quick quantize clip starts', hint: 'Shift+Q', fn: quantStarts }, { label: 'Render armed mixer buses to WAV', hint: 'Alt+R in the Mixer', fn: () => bus.emit('renderArmed') }] },
      { label: 'Snap', sub: SNAPS().map(([v, l]) => ({ label: l, on: Math.abs(prefs.songSnap - v) < 1e-9, fn: () => { prefs.songSnap = v; savePrefs(); build(); } })) },
      { label: 'Select', sub: [
        { label: 'Deselect', fn: () => selectWhere(() => false) }, { label: 'Select all', hint: 'Ctrl+A', fn: () => selectWhere(() => true) }, { label: 'Select by source', hint: 'Shift+C', disabled: !n, fn: bySource },
        { label: 'Select muted', fn: () => selectWhere(c => c.mute) }, { label: 'Invert selection', fn: () => { const was = new Set(picked); selectWhere(c => !was.has(c.id)); } },
        { label: 'Select time around selection', disabled: !n, fn: () => { S.songSel = { a: Math.min(...g.map(c => c.start)), b: Math.max(...g.map(c => c.start + c.len)) }; build(); } }] },
      { label: 'Group', sub: [{ label: 'Group', hint: 'Shift+G', fn: group }, { label: 'Ungroup', hint: 'Alt+G', fn: ungroup }] },
      { label: 'Zoom', sub: [{ label: 'Zoom in', fn: () => zoom(1.33) }, { label: 'Zoom out', fn: () => zoom(0.75) }, { label: 'Zoom out far (whole song)', fn: () => zoomFit() },
        { label: 'Zoom on selection', disabled: !n, fn: () => zoomFit([Math.min(...g.map(c => c.start)), Math.max(...g.map(c => c.start + c.len))]) }] },
      { label: 'Time markers', sub: [{ label: 'Add one at the play marker', hint: 'Alt+T', fn: addMarker },
        { label: 'Loop the time selection', disabled: !S.songSel, fn: () => { S.songStart = S.songSel.a; toast('Playback starts at the selection; the song loops at its end'); build(); } },
        { label: 'Clear the time selection', disabled: !S.songSel, fn: () => { S.songSel = null; build(); } },
        ...(S.p.song.markers || []).map(m => ({ label: 'Delete “' + m.name + '”', fn: () => { S.p.song.markers = S.p.song.markers.filter(x => x !== m); commit(); } }))] },
      { label: 'Arrangements', sub: arrMenu() },
    ];
  }
  function clipMenu(c, x, y) {
    const ids = withGroups(picked.has(c.id) ? new Set(picked) : new Set([c.id])), cs = all().filter(z => ids.has(z.id)), p = !isAudio(c) && patOf(c);
    menu(x, y, [{ head: isAudio(c) ? c.name || 'Audio clip' : p.name },
      { label: c.mute ? 'Unmute' : 'Mute', fn: () => { cs.forEach(z => { z.mute = !c.mute; }); commit(); } }, { label: 'Colour', sub: colourItems(cs) },
      p && { label: 'Rename pattern…', fn: async () => { const v = await ask('Pattern name', p.name); if (v) { p.name = v; commit(); } } },
      p && { label: 'Make unique', fn: () => makeUnique(cs) }, { label: 'Select all from this source', fn: () => { picked.clear(); picked.add(c.id); bySource(); } },
      { label: cs.some(z => z.group) ? 'Ungroup' : 'Group with the selection', fn: () => { cs.forEach(z => { picked.add(z.id); }); cs.some(z => z.group) ? ungroup() : group(); } },
      { label: 'Duplicate', fn: () => { picked.clear(); cs.forEach(z => picked.add(z.id)); duplicate(); } }, '-', { label: 'Delete', fn: () => drop(ids) }]);
  }
  function build() {
    const P = S.p; bars = Math.max(32, songBars(P) + 8); arrangements();
    for (const id of picked) if (!all().some(c => c.id === id)) picked.delete(id);
    toolbar();
    const setPatColour = (p, c) => { if (c) p.color = c; else delete p.color; commit(); };
    const o = prefs.picker || {}, src = S.songSrc && audioSources().find(a => a.sample === S.songSrc.audio);
    if (!src) S.songSrc = null;
    const order = list => { const k = o.sort || 'order', by = { name: x => x.name || '', colour: x => x.col, len: x => -x.len }[k]; return k === 'order' ? list : [...list].sort((a, b) => (by(a) < by(b) ? -1 : by(a) > by(b) ? 1 : 0)); };
    const drag = key => ({ draggable: true, ondragstart: e => { e.dataTransfer.setData('text/x-undae-pick', key); e.dataTransfer.effectAllowed = 'copy'; } });
    const pats = o.pats === false ? [] : order(P.patterns.filter(p => !o.used || P.song.clips.some(c => c.pat === p.id)).map(p => ({ p, name: p.name, col: colorOf(p.id), len: p.bars })));
    const auds = o.audio === false ? [] : order(audioSources().map(a => ({ a, name: a.name || 'Audio', col: a.color || '', len: a.len })));
    pal.style.display = prefs.songPicker === false ? 'none' : '';
    pal.replaceChildren(h('div', { class: 'phead2' }, h('b', null, 'Picker'), btn('▾', e => menuAt(e.currentTarget, pickerMenu()), 'sm', 'What the picker shows and how it is sorted')),
      ...pats.map(({ p }) => h('button', { class: 'pchip' + (p.id === S.pat && !S.songSrc ? ' on' : ''), 'data-src': 'pat:' + p.id, style: `--c:${colorOf(p.id)}`, title: 'Click to draw with this pattern, or drag it onto a track', ...drag('pat:' + p.id), onclick: () => { S.songSrc = null; S.pat = p.id; bus.emit('sel'); build(); },
      oncontextmenu: e => { e.preventDefault(); menu(e.clientX, e.clientY, [{ head: p.name }, { label: 'Rename…', fn: async () => { const v = await ask('Pattern name', p.name); if (v) { p.name = v; commit(); } } },
        { label: 'Colour', sub: [...CLIP_COLORS.map(([c, n]) => ({ label: '● ' + n, fn: () => setPatColour(p, c) })), { label: 'Default colour', fn: () => setPatColour(p, null) }] },
        { label: 'Select its clips', fn: () => selectWhere(c => c.pat === p.id) }]); } }, h('i'), p.name, h('small', null, Math.round(p.bars * BAR / BEAT) + ' beats'))),
      btn('+ Pattern', async () => { const v = await ask('Pattern name', 'Pattern ' + (P.patterns.length + 1)); if (v) { const p = newPattern(v); P.patterns.push(p); S.pat = p.id; commit(); } }, 'sm'),
      ...(auds.length ? [h('div', { class: 'phead2' }, h('small', null, 'Audio clips'))] : []),
      ...auds.map(({ a }) => h('button', { class: 'pchip audio' + (S.songSrc && S.songSrc.audio === a.sample ? ' on' : ''), 'data-src': 'audio:' + a.sample, style: `--c:${clipColor(a)}`, title: 'Click to draw with this audio clip, or drag it onto a track', ...drag('audio:' + a.sample),
        onclick: () => { S.songSrc = S.songSrc && S.songSrc.audio === a.sample ? null : { audio: a.sample }; build(); },
        oncontextmenu: e => { e.preventDefault(); const mine = c => c.sample === a.sample; menu(e.clientX, e.clientY, [{ head: a.name || 'Audio' },
          { label: 'Rename…', fn: async () => { const v = await ask('Clip name', a.name || 'Audio'); if (v) { S.p.song.audio.filter(mine).forEach(c => { c.name = v; }); commit(); } } },
          { label: 'Colour', sub: colourItems(S.p.song.audio.filter(mine)) }, { label: 'Select its clips', fn: () => selectWhere(mine) },
          { label: 'Delete all its clips', fn: () => drop(new Set(S.p.song.audio.filter(mine).map(c => c.id))) }]); } }, h('i'), '〜 ' + (a.name || 'Audio'), h('small', null, (a.len * barSec()).toFixed(1) + ' s'))));
    size();
    const sub = prefs.songSnap && prefs.songSnap < 1 && bw * prefs.songSnap >= 7 ? bw * prefs.songSnap : bw;
    lanes.style.setProperty('--sub', sub + 'px');
    ruler.replaceChildren(...Array.from({ length: bars }, (_, i) => h('span', { style: `left:${i * bw}px` }, bw < 30 && i % 4 ? '' : String(i + 1))),
      ...(prefs.songMarkers === false ? [] : P.song.markers || []).map(m => h('b', { class: 'mk', style: `left:${m.t * bw}px`, title: 'Time marker: click to jump · right-click to rename or delete', onpointerdown: e => e.stopPropagation(),
        onclick: () => { S.songStart = m.t; mark.style.transform = `translateX(${m.t * bw}px)`; },
        oncontextmenu: e => { e.preventDefault(); e.stopPropagation(); menu(e.clientX, e.clientY, [{ head: m.name }, { label: 'Rename…', fn: async () => { const v = await ask('Marker name', m.name); if (v) { m.name = v; commit(); } } }, { label: 'Delete', fn: () => { P.song.markers = P.song.markers.filter(x => x !== m); commit(); } }]); } }, m.name)),
      S.songSel ? h('i', { class: 'ssel', style: `left:${S.songSel.a * bw}px;width:${(S.songSel.b - S.songSel.a) * bw}px` }) : '');
    selBox.style.display = S.songSel ? 'block' : 'none'; if (S.songSel) Object.assign(selBox.style, { left: S.songSel.a * bw + 'px', width: (S.songSel.b - S.songSel.a) * bw + 'px' });
    lanes.style.height = P.song.tracks * TH + 'px'; lanes.style.setProperty('--th', TH + 'px');
    autos.style.display = prefs.songAutos === false ? 'none' : '';
    live.clear();
    lanes.replaceChildren(...all().map(clipNode).filter(Boolean), box);
    autos.replaceChildren(...P.song.auto.map(autoLane).filter(Boolean));
    mark.style.transform = `translateX(${S.songStart * bw}px)`;
  }
  // One automation lane: a line through draggable points. Click to add a point, drag to move, right-click a point to remove it.
  function autoLane(l) {
    const tg = engine.autoTarget(l.target); if (!tg) return null;
    const svg = document.createElementNS(NS, 'svg'), W = () => bars * bw, x = t => t / BAR * bw, y = v => 4 + (1 - (v - tg.min) / (tg.max - tg.min)) * (AH - 8);
    const draw = () => {
      svg.setAttribute('width', W()); svg.setAttribute('height', AH);
      const p = l.pts; svg.innerHTML = (p.length ? `<polyline points="0,${y(p[0].v)} ${p.map(q => `${x(q.t)},${y(q.v)}`).join(' ')} ${W()},${y(p[p.length - 1].v)}" fill="none" stroke="currentColor" stroke-width="2"/>` : '') + p.map((q, i) => `<circle cx="${x(q.t)}" cy="${y(q.v)}" r="4.5" data-i="${i}"/>`).join('');
    };
    draw();
    svg.addEventListener('pointerdown', e => {
      if (e.button) return;
      const r = svg.getBoundingClientRect(), at = ev => ({ t: Math.max(0, Math.round(snap((ev.clientX - r.left) / bw, ev) * BAR)), v: clamp(tg.min + (1 - (ev.clientY - r.top - 4) / (AH - 8)) * (tg.max - tg.min), tg.min, tg.max) });
      const i = e.target.dataset ? e.target.dataset.i : null; let p;
      if (i != null) p = l.pts[+i]; else { p = at(e); l.pts.push(p); l.pts.sort((a, b) => a.t - b.t); }
      try { svg.setPointerCapture(e.pointerId); } catch { /* synthetic pointer */ }
      const mv = ev => { Object.assign(p, at(ev)); l.pts.sort((a, b) => a.t - b.t); draw(); };
      const up = () => { svg.removeEventListener('pointermove', mv); svg.removeEventListener('pointerup', up); svg.removeEventListener('pointercancel', up); commit(''); };
      svg.addEventListener('pointermove', mv); svg.addEventListener('pointerup', up); svg.addEventListener('pointercancel', up);
      draw();
    });
    svg.addEventListener('contextmenu', e => { e.preventDefault(); const i = e.target.dataset ? e.target.dataset.i : null; if (i != null && l.pts.length > 1) { l.pts.splice(+i, 1); draw(); commit(''); } });
    return h('div', { class: 'alane', style: `height:${AH}px` }, svg,
      h('button', { class: 'alab', title: 'Automation lane options', onclick: e => menuAt(e.currentTarget, [{ head: tg.label },
        { label: 'Flatten to the current value', fn: () => { l.pts = [{ t: 0, v: tg.obj[tg.key] }]; commit(); } },
        { label: 'Delete lane', fn: () => { S.p.song.auto = S.p.song.auto.filter(z => z !== l); commit(); } }]) }, tg.label));
  }
  function size() { inner.style.width = bars * bw + 'px'; inner.style.setProperty('--bw', bw + 'px'); }
  function zoom(k, x) {
    const vx = x ?? scroll.clientWidth / 2, at = (scroll.scrollLeft + vx) / bw;
    bw = clamp(bw * k, 10, 480); build(); scroll.scrollLeft = at * bw - vx;
  }
  pinch(scroll, (k, cx) => zoom(k, cx - scroll.getBoundingClientRect().left), (dx, dy) => { scroll.scrollLeft -= dx; scroll.scrollTop -= dy; });
  scroll.addEventListener('wheel', e => {
    if (e.altKey && !e.ctrlKey) { e.preventDefault(); setTH(TH * (e.deltaY < 0 ? 1.1 : 1 / 1.1)); return; }
    if (!(e.ctrlKey || e.metaKey)) return;
    e.preventDefault(); zoom(e.deltaY < 0 ? 1.2 : 1 / 1.2, e.clientX - scroll.getBoundingClientRect().left);
  }, { passive: false });

  // Ruler: click sets the play marker; drag makes a time selection (for Insert / Delete space and looping).
  ruler.addEventListener('pointerdown', e => {
    if (e.button) return;
    const r = ruler.getBoundingClientRect(), x0 = e.clientX, at = ev => Math.max(0, snap((ev.clientX - r.left) / bw, ev));
    try { ruler.setPointerCapture(e.pointerId); } catch { /* synthetic pointer */ }
    let moved = false;
    const mv = ev => { if (Math.abs(ev.clientX - x0) < 4 && !moved) return; moved = true; const a = at(e), b = at(ev); S.songSel = { a: Math.min(a, b), b: Math.max(a, b) }; selBox.style.display = 'block'; Object.assign(selBox.style, { left: S.songSel.a * bw + 'px', width: (S.songSel.b - S.songSel.a) * bw + 'px' }); };
    const up = ev => {
      ruler.removeEventListener('pointermove', mv); ruler.removeEventListener('pointerup', up);
      if (!moved) { S.songSel = null; S.songStart = Math.max(0, Math.floor((ev.clientX - r.left) / bw)); } else S.songStart = S.songSel.a;
      build();
    };
    ruler.addEventListener('pointermove', mv); ruler.addEventListener('pointerup', up);
  });
  const drop = ids => { const P = S.p.song; P.clips = P.clips.filter(c => !ids.has(c.id)); P.audio = P.audio.filter(c => !ids.has(c.id)); ids.forEach(i => picked.delete(i)); commit(); };
  const paint = () => { for (const n of lanes.querySelectorAll('.clip')) n.classList.toggle('picked', picked.has(n.dataset.id)); };
  lanes.addEventListener('contextmenu', e => {
    const node = e.target.closest('.clip'); if (!node) return;
    e.preventDefault();
    const c = all().find(x => x.id === node.dataset.id), ids = withGroups(picked.has(c.id) ? new Set(picked) : new Set([c.id]));
    if (tool === 'zoom') return zoom(1 / 1.5, e.clientX - scroll.getBoundingClientRect().left);
    if (tool === 'select') return clipMenu(c, e.clientX, e.clientY);
    if (!isAudio(c)) return touch.last === 'touch' ? clipMenu(c, e.clientX, e.clientY) : drop(ids);   // a long-press never deletes
    menu(e.clientX, e.clientY, [{ head: c.name || 'Audio clip' },
      { label: 'Rename…', fn: async () => { const v = await ask('Clip name', c.name || 'Audio'); if (v) { c.name = v; commit(); } } },
      { label: 'Mute this clip', on: !!c.mute, fn: () => { c.mute = !c.mute; commit(); } }, { label: 'Colour', sub: colourItems([...ids].map(i => all().find(z => z.id === i)).filter(Boolean)) },
      ...S.p.buses.map(b => ({ label: 'Play through ' + busLabel(S.p, b.id) + ' · ' + b.name, on: c.bus === b.id, fn: () => { c.bus = b.id; commit(); } })),
      '-', { label: 'Stretch to project tempo (keeps pitch)', on: c.stretch !== false, fn: () => { const r0 = engine.ratio(c); c.stretch = c.stretch === false; c.bpm ||= S.p.bpm; c.len *= engine.ratio(c) / r0; commit(); } },
      { label: 'Delete', fn: () => drop(ids) }]);
  });
  let emptyDown = false;
  lanes.addEventListener('click', e => {
    if (!emptyDown || e.target !== lanes || e.ctrlKey || e.metaKey || tool !== 'draw') return;
    if (picked.size) { picked.clear(); paint(); return; }
    const r = lanes.getBoundingClientRect(), p = pat(), track = Math.floor((e.clientY - r.top) / TH), start = Math.max(0, snap((e.clientX - r.left) / bw, e, true));
    const src = S.songSrc && audioSources().find(a => a.sample === S.songSrc.audio);
    if (src) audioClipAt(src, track, start); else S.p.song.clips.push({ id: uid(), pat: p.id, track, start, len: p.bars });
    commit();
  });
  lanes.addEventListener('contextmenu', e => { if (tool === 'zoom' && !e.target.closest('.clip')) { e.preventDefault(); zoom(1 / 1.5, e.clientX - scroll.getBoundingClientRect().left); } });
  // Tools other than draw and select: delete, mute, slice, slip, paint, zoom, play.
  function toolDown(e) {
    if (!['erase', 'mute', 'slice', 'paint', 'slip', 'zoom', 'play'].includes(tool)) return false;
    const r = lanes.getBoundingClientRect(), xb = ev => (ev.clientX - r.left) / bw, tr = ev => clamp(Math.floor((ev.clientY - r.top) / TH), 0, S.p.song.tracks - 1);
    const at = ev => { const n = document.elementFromPoint(ev.clientX, ev.clientY), node = n && n.closest && n.closest('.clip'); return node && all().find(c => c.id === node.dataset.id); };
    const nodeOf = c => lanes.querySelector(`.clip[data-id="${c.id}"]`);
    const follow = (mv, up) => {
      try { lanes.setPointerCapture(e.pointerId); } catch { /* synthetic pointer */ }
      const u = ev => { lanes.removeEventListener('pointermove', mv); lanes.removeEventListener('pointerup', u); lanes.removeEventListener('pointercancel', u); if (up) up(ev); };
      lanes.addEventListener('pointermove', mv); lanes.addEventListener('pointerup', u); lanes.addEventListener('pointercancel', u);
    };
    if (tool === 'zoom') { zoom(1.5, e.clientX - scroll.getBoundingClientRect().left); return true; }
    if (tool === 'play') { playFrom(Math.max(0, snap(xb(e), e, true))); return true; }
    if (tool === 'erase') {
      const gone = new Set(), hit = ev => { const c = at(ev); if (c && !gone.has(c.id)) withGroups(new Set([c.id])).forEach(i => { gone.add(i); const n = lanes.querySelector(`.clip[data-id="${i}"]`); if (n) n.remove(); }); };
      hit(e); follow(hit, () => { if (gone.size) drop(gone); }); return true;
    }
    if (tool === 'mute') {
      const first = at(e); if (!first) return true;
      const v = !first.mute, done = new Set(), hit = ev => { const c = at(ev); if (c && !done.has(c.id)) { done.add(c.id); c.mute = v; const n = nodeOf(c); if (n) n.classList.toggle('muted', v); } };
      hit(e); follow(hit, () => commit()); return true;
    }
    if (tool === 'slice') { const c = at(e); if (c) sliceClip(c, snap(xb(e), e)); return true; }
    if (tool === 'slip') {
      const c = at(e); if (!c) return true; const x0 = e.clientX, o0 = c.off || 0;
      follow(ev => {
        const d = (ev.clientX - x0) / bw;
        if (isAudio(c)) c.off = Math.max(0, o0 - d * barSec() / engine.ratio(c)); else { const pl = patOf(c).bars; c.off = (((o0 - snap(d, ev)) % pl) + pl) % pl; }
        const n = nodeOf(c); if (n) n.replaceWith(clipNode(c));
      }, () => commit());
      return true;
    }
    if (tool === 'paint') {   // drag right along a track: copies of the clip under the pointer (or the picked pattern), one after another
      const asrc = !at(e) && S.songSrc && audioSources().find(a => a.sample === S.songSrc.audio);
      if (asrc) { const trk = tr(e), x0 = Math.max(0, snap(xb(e), e, true)), made = [], put = k => { const z = audioClipAt(asrc, trk, x0 + k * asrc.len); made.push(z); lanes.append(clipNode(z)); }; put(0); follow(ev => { while (x0 + made.length * asrc.len < xb(ev)) put(made.length); }, () => commit()); return true; }
      const c0 = at(e), p = c0 && !isAudio(c0) ? patOf(c0) : pat(); if (!p) return true;
      const trk = c0 ? c0.track : tr(e), len = c0 && !isAudio(c0) ? c0.len : p.bars, x0 = c0 ? c0.start + c0.len : Math.max(0, snap(xb(e), e, true)), made = [];
      const place = k => { const z = { id: uid(), pat: p.id, track: trk, start: x0 + k * len, len, ...(c0 && c0.color ? { color: c0.color } : {}) }; S.p.song.clips.push(z); made.push(z); lanes.append(clipNode(z)); };
      if (!c0) place(0);
      follow(ev => { const end = xb(ev); while (x0 + made.length * len < end) place(made.length); }, () => { if (made.length) commit(); });
      return true;
    }
    return false;
  }
  lanes.addEventListener('pointerdown', e => {
    if (e.button) return;
    scroll.focus({ preventScroll: true });
    if (toolDown(e)) return;
    const node = e.target.closest('.clip'), mod = e.ctrlKey || e.metaKey || (tool === 'select' && !node), x0 = e.clientX, y0 = e.clientY;
    const track = fn => { const up = () => { lanes.removeEventListener('pointermove', fn); lanes.removeEventListener('pointerup', up); lanes.removeEventListener('pointercancel', up); done(); }; let done = () => {}; lanes.addEventListener('pointermove', fn); lanes.addEventListener('pointerup', up); lanes.addEventListener('pointercancel', up); return f => { done = f; }; };
    emptyDown = !node && !mod;
    if (emptyDown) return;
    try { lanes.setPointerCapture(e.pointerId); } catch { /* synthetic pointer */ }
    if (!node) {
      // Ctrl+drag on empty space: rubber-band selection.
      const r = lanes.getBoundingClientRect(), base = e.shiftKey ? new Set(picked) : new Set();
      track(ev => {
        const ax = Math.min(x0, ev.clientX) - r.left, bx = Math.max(x0, ev.clientX) - r.left, ay = Math.min(y0, ev.clientY) - r.top, by = Math.max(y0, ev.clientY) - r.top;
        Object.assign(box.style, { display: 'block', left: ax + 'px', top: ay + 'px', width: bx - ax + 'px', height: by - ay + 'px' });
        picked.clear(); base.forEach(i => picked.add(i));
        for (const c of all()) if (c.start * bw < bx && (c.start + c.len) * bw > ax && c.track * TH < by && (c.track + 1) * TH > ay) picked.add(c.id);
        paint();
      })(() => { box.style.display = 'none'; });
      return;
    }
    const c = all().find(x => x.id === node.dataset.id), r = node.getBoundingClientRect(), resize = e.clientX > r.right - 10;
    if (mod) { picked.has(c.id) ? picked.delete(c.id) : picked.add(c.id); paint(); return; }
    if (!picked.has(c.id)) { picked.clear(); withGroups(new Set([c.id])).forEach(i => picked.add(i)); paint(); }
    const group = all().filter(x => picked.has(x.id)), orig = new Map(group.map(x => [x.id, { ...x }])), o = orig.get(c.id);
    const minS = Math.min(...group.map(x => x.start)), minT = Math.min(...group.map(x => x.track)), maxT = Math.max(...group.map(x => x.track));
    let moved = false;
    track(ev => {
      const dx = (ev.clientX - x0) / bw, dt = clamp(Math.round((ev.clientY - y0) / TH), -minT, S.p.song.tracks - 1 - maxT);
      if (resize) {
        const s = grid(ev) || 1 / 16, max = isAudio(c) ? ((samples.get(c.sample) || { buffer: { duration: 0 } }).buffer.duration - (c.off || 0)) * engine.ratio(c) / barSec() : Infinity;
        c.len = clamp(snap(o.start + o.len + dx, ev) - o.start, Math.min(s, max), max);
        moved = moved || c.len !== o.len;
      } else {
        const d = Math.max(-minS, snap(o.start + dx, ev) - o.start);
        for (const x of group) { const q = orig.get(x.id); x.start = q.start + d; x.track = q.track + dt; }
        moved = moved || d !== 0 || dt !== 0;
      }
      for (const x of group) { const n = lanes.querySelector(`.clip[data-id="${x.id}"]`); if (n) place(n, x); }
    })(() => { if (moved) commit(); else if (!isAudio(c) && S.pat !== c.pat) { S.pat = c.pat; bus.emit('sel'); } });
  });
  scroll.addEventListener('keydown', e => {
    const id = e.key === 'Backspace' ? 'song.del' : actionFor(e, 'song'); if (!id) return;
    const acts = {
      'song.del': () => { if (picked.size) drop(new Set(picked)); }, 'song.all': () => selectWhere(() => true), 'song.none': () => { picked.clear(); S.songSel = null; build(); },
      'song.dup': duplicate, 'song.source': bySource, 'song.group': group, 'song.ungroup': ungroup, 'song.insert': insertSpace, 'song.space': deleteSpace, 'song.quant': quantStarts, 'song.marker': addMarker,
      'song.copy': copy, 'song.cut': () => { copy(); if (picked.size) drop(new Set(picked)); }, 'song.paste': paste,
    };
    if (id.startsWith('song.t.')) setTool(id.slice(7)); else if (acts[id]) acts[id](); else return;
    e.preventDefault(); e.stopPropagation();
  });

  // Drop audio from the library, a sample folder or the computer straight onto a track.
  lanes.addEventListener('dragover', e => e.preventDefault());
  lanes.addEventListener('drop', async e => {
    e.preventDefault();
    const r = lanes.getBoundingClientRect(), track = clamp(Math.floor((e.clientY - r.top) / TH), 0, S.p.song.tracks - 1); let start = Math.max(0, snap((e.clientX - r.left) / bw, e, true));
    const pick = e.dataTransfer.getData('text/x-undae-pick');   // dragged from the picker panel
    if (pick) {
      const [k, id] = pick.split(':'), p = k === 'pat' && S.p.patterns.find(q => q.id === id), a = k === 'audio' && audioSources().find(q => q.sample === id);
      if (p) S.p.song.clips.push({ id: uid(), pat: p.id, track, start, len: p.bars }); else if (a) audioClipAt(a, track, start);
      return commit();
    }
    for (const id of await dropped(e)) { const smp = samples.get(id); if (!smp) continue; const len = smp.buffer.duration / barSec(); S.p.song.audio.push({ id: uid(), sample: id, name: smp.name.slice(0, 24), track, start, len, off: 0, bus: 'master', gain: 1, bpm: S.p.bpm, stretch: false }); start += len; }
    commit();
  });
  // Takes being recorded right now: a clip that grows under the playhead with its waveform.
  function drawLive(t) {
    for (const rec of engine.recs) {
      let L = live.get(rec);
      if (!L) { const cv = h('canvas', { height: TH - 20 }); L = { cv, node: h('div', { class: 'clip audio rec', style: `height:${TH - 4}px;--c:#e5484d` }, h('b', null, '● ' + rec.name), cv) }; live.set(rec, L); lanes.append(L.node); }
      const o = engine.takeStart(rec), a = o.tick / BAR, w = Math.max(2, (rec.len / rec.sr - o.off) / barSec() * bw);
      L.node.style.left = a * bw + 'px'; L.node.style.top = rec.track * TH + 2 + 'px'; L.node.style.width = w + 'px';
      const W = Math.min(4000, Math.ceil(w)); if (L.cv.width !== W) L.cv.width = W;
      const g = L.cv.getContext('2d'), n = rec.peaks.length, hh = TH - 20; g.clearRect(0, 0, W, hh); g.fillStyle = 'rgba(0,0,0,.6)';
      for (let x = 0; x < W; x++) { const y = Math.max(1, (rec.peaks[Math.floor(x / W * n)] || 0) * hh); g.fillRect(x, (hh - y) / 2, 1, y); }
    }
    for (const [rec, L] of live) if (!engine.recs.includes(rec)) { L.node.remove(); live.delete(rec); }
  }
  bus.on('pos', t => {
    if (!el.isConnected) return;
    const show = t >= 0 && S.mode === 'song';
    head.style.display = show ? 'block' : 'none';
    if (!show) { if (live.size) drawLive(0); return; }
    const x = t / BAR * bw;
    if (x > (bars - 4) * bw) { bars += 16; size(); ruler.append(...Array.from({ length: 16 }, (_, i) => h('span', { style: `left:${(bars - 16 + i) * bw}px` }, String(bars - 15 + i)))); }
    head.style.transform = `translateX(${x}px)`;
    if (x > scroll.scrollLeft + scroll.clientWidth - 30 || x < scroll.scrollLeft) scroll.scrollLeft = Math.max(0, x - 40);
    if (engine.recs.length || live.size) drawLive(t);
  });
  bus.on('project', build);
  bus.on('samples', () => el.isConnected && build());
  bus.on('sel', () => { for (const b of pal.querySelectorAll('.pchip')) b.classList.toggle('on', S.songSrc ? b.dataset.src === 'audio:' + S.songSrc.audio : b.dataset.src === 'pat:' + S.pat); });
  build();
  return { el, show: build };
}
