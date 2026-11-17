// Channel rack: step sequencer, with note-overview and waveform row views.
import { h, sel, knob, icon, bus, menu, menuAt, ask, toast, pickFile, uid, clamp, cssVar, STEP, BAR, BEAT, PPQ } from '../util.js';
import { choosePlugin, slotFor } from './plugui.js';
import { actionFor } from '../keys.js';
import { S, pat, notesOf, commit, newChannel, COLORS, busLabel, prefs, savePrefs, addBus, selectedChannels } from '../state.js';
import { engine, swingShift } from '../audio/engine.js';
import { addFile, samples, dropped } from '../samples.js';
import { waveShape } from '../audio/synth.js';

export default function Rack() {
  const bar = h('div', { class: 'vbar' }), rows = h('div', { class: 'rows' }), el = h('div', { class: 'view rack', tabIndex: 0 }, bar, rows);
  const phead = h('i', { class: 'rplay' }); let playTick = -1;   // playhead of the Notes / Wave views
  const sync = () => engine.graph.sync();
  let lastPat = null;
  const chOf = id => S.p.channels.find(c => c.id === id);
  const stepsNow = () => Math.round(pat().bars * BAR / STEP);

  function toolbar() {
    const p = pat(), beats = Math.round(p.bars * BAR / BEAT), max = Math.round(64 * BAR / BEAT);
    // Pattern length in beats: type a number or scroll over the box.
    const setBeats = v => { pat().bars = clamp(Math.round(v) || 1, 1, max) * BEAT / BAR; commit(); };
    const len = h('input', { class: 'bpm beats', type: 'number', min: 1, max, value: beats, title: 'Pattern length in beats - type a number or scroll', onchange: () => setBeats(+len.value), onkeydown: e => e.stopPropagation() });
    len.addEventListener('wheel', e => { e.preventDefault(); setBeats(beats + (e.deltaY < 0 ? 1 : -1)); }, { passive: false });
    bar.replaceChildren(
      h('label', null, 'Beats'), len,
      h('div', { class: 'seg', title: 'What each channel row shows' }, [['steps', 'Steps'], ['roll', 'Notes'], ['wave', 'Wave']].map(([id, l]) => h('button', { class: prefs.rackView === id ? 'on' : '', onclick: () => { prefs.rackView = id; savePrefs(); build(); } }, l))),
      knob({ label: 'Swing', size: 'sm', value: S.p.swing, def: 0, fmt: v => Math.round(v * 100) + '%', onInput: v => { S.p.swing = v; applySwing(); }, onChange: () => commit('') }),
      h('label', { title: 'Which notes are pushed late: every second 16th, or every second 8th' }, 'Swing grid'), sel([[16, '1/16'], [8, '1/8']], S.p.swingDiv || 16, v => { S.p.swingDiv = +v; applySwing(); commit(''); }),
      ...groupFilter(),
      h('span', { class: 'grow' }),
      h('button', { class: 'btn primary', onclick: e => { const at = e.currentTarget; menuAt(at, [
        { label: 'Synth channel', fn: () => addCh(newChannel('synth')) },
        { label: 'Keys sampler', fn: () => addCh(newChannel('sample', { name: 'Keys', gate: true, rel: 0.3 })) },
        { label: 'Plug-in instrument (CLAP / VST)…', fn: () => choosePlugin(at, 'inst', pl => addCh(newChannel('plugin', { name: pl.name.slice(0, 18), plug: slotFor(pl, true) }))) },
        { label: 'Upload audio samples…', fn: upload },
        { label: 'Browse 808 kit & library', fn: () => bus.emit('goto', { to: 'lib', from: 'rack' }) },
        { label: 'Design a new sound', fn: () => bus.emit('goto', { to: 'design', from: 'rack' }) },
      ]); } }, '+ Channel'));
  }
  // Slide the swung steps sideways so the groove is visible as well as audible.
  function applySwing() { for (let i = 1; i < 4; i++) rows.style.setProperty('--sw' + i, (swingShift(S.p, i * STEP) / STEP).toFixed(3)); }
  function addCh(c) { S.p.channels.push(c); S.ch = c.id; commit(); }
  async function upload() {
    for (const f of await pickFile('audio/*', true)) {
      try { const id = await addFile(f); addCh(newChannel('sample', { name: f.name.replace(/\.[^.]+$/, '').slice(0, 18), sample: id })); }
      catch { toast('Could not decode ' + f.name); }
    }
  }

  function row(c, steps) {
    const view = prefs.rackView;
    let body;
    if (view === 'steps') {
      const bs = Math.max(1, BEAT / STEP), spb = BAR / STEP;
      body = h('div', { class: 'steps' });
      for (let i = 0; i < steps; i++) body.append(h('button', { class: 'st q' + (i & 3) + (Math.floor(i / bs) & 1 ? ' alt' : '') + (i % bs === 0 ? ' beat' : '') + (i % spb === 0 && i ? ' bar' : ''), 'data-i': i, 'aria-label': 'step ' + (i + 1) }));
    } else {
      body = h('canvas', { class: 'rview', width: Math.min(4000, steps * 22), height: 36, title: view === 'roll' ? 'Notes in this pattern - click to edit them in the piano roll' : 'Sound of this channel across the pattern - click to hear it',
        onclick: () => { pick(c.id); if (view === 'roll') bus.emit('goto', { to: 'roll', from: 'rack' }); else engine.preview(c, c.type === 'synth' ? 60 : c.root, 0.85, 0.5); } });
    }
    const led = h('button', { class: 'led' + (c.mute ? '' : ' on'), title: 'Mute', onclick: () => { c.mute = !c.mute; led.classList.toggle('on', !c.mute); sync(); commit(''); } });
    const solo = h('button', { class: 'mini' + (c.solo ? ' on' : ''), title: 'Solo', onclick: () => { c.solo = !c.solo; solo.classList.toggle('on', c.solo); sync(); commit(''); } }, 'S');
    const setBus = id => { c.bus = id; busB.textContent = busLabel(S.p, id); sync(); commit(''); };
    const busB = h('button', { class: 'busno', title: 'Mixer bus this channel plays through - click to choose, scroll to step',
      onclick: e => menuAt(e.currentTarget, [{ head: 'Send ' + c.name + ' to' }, ...S.p.buses.map(b => ({ label: busLabel(S.p, b.id) + ' · ' + b.name, on: c.bus === b.id, fn: () => setBus(b.id) }))]) }, busLabel(S.p, c.bus));
    busB.addEventListener('wheel', e => { e.preventDefault(); const bs = S.p.buses, i = Math.max(0, bs.findIndex(b => b.id === c.bus)), j = clamp(i + (e.deltaY > 0 ? 1 : -1), 0, bs.length - 1); if (j !== i) setBus(bs[j].id); }, { passive: false });
    return h('div', { class: 'row' + (c.id === S.ch ? ' sel' : ''), 'data-id': c.id, style: `--c:${c.color}` },
      h('div', { class: 'rhead' }, led, solo, busB,
        knob({ label: 'Pan', size: 'xs', min: -1, max: 1, bi: true, value: c.pan, def: 0, link: `ch:${c.id}:pan`, onInput: v => { c.pan = v; sync(); engine.autoWrite(`ch:${c.id}:pan`, v); }, onChange: () => commit('') }),
        knob({ label: 'Vol', size: 'xs', max: 1.25, value: c.vol, def: 0.8, link: `ch:${c.id}:vol`, onInput: v => { c.vol = v; sync(); engine.autoWrite(`ch:${c.id}:vol`, v); }, onChange: () => commit('') }),
        h('button', { class: 'cname', title: 'Click: select · Ctrl+click: select several · double-click: edit instrument · right-click: more · drop audio here to replace the sound', onclick: e => pick(c.id, e), ondblclick: () => bus.emit('goto', { to: 'inst', from: 'rack' }), oncontextmenu: e => { e.preventDefault(); rowMenu(c, e.clientX, e.clientY); } }, h('i'), h('span', null, c.name)),
        h('button', { class: 'mini', title: 'Open in piano roll', onclick: () => { pick(c.id); bus.emit('goto', { to: 'roll', from: 'rack' }); } }, icon('roll')),
        h('button', { class: 'mini', title: 'Channel options', onclick: e => { const r = e.currentTarget.getBoundingClientRect(); rowMenu(c, r.left, r.bottom); } }, '⋯')),
      body);
  }
  function pick(id, e) {
    if (e && (e.ctrlKey || e.metaKey)) {   // Ctrl+click: add to or remove from the selected channels (for Ctrl+L, Alt+C, Alt+G, Alt+Del)
      const s = new Set(S.chSel && S.chSel.length ? S.chSel : [S.ch]); s.has(id) ? s.delete(id) : s.add(id); S.chSel = [...s];
      for (const r of rows.children) r.classList.toggle('multi', s.has(r.dataset.id)); return;
    }
    if (S.chSel && S.chSel.length) { S.chSel = []; for (const r of rows.children) r.classList.remove('multi'); }
    if (S.ch === id) return; S.ch = id; bus.emit('sel');
  }
  // Channel groups (Alt+G names the selected channels' group): the filter shows one group, the unsorted channels, or all.
  // The choice is kept in the project (S.p.rackGroup: '' all, '-' unsorted, otherwise a group name).
  const groups = () => [...new Set(S.p.channels.map(c => c.group).filter(Boolean))].sort();
  const shown = () => { const g = S.p.rackGroup || ''; if (g && g !== '-' && !groups().includes(g)) S.p.rackGroup = ''; const k = S.p.rackGroup || ''; return S.p.channels.filter(c => !k || (k === '-' ? !c.group : c.group === k)); };
  function groupFilter() {
    const gs = groups(); if (!gs.length) return [];
    return [h('label', { title: 'Show one group of channels (Alt+G groups the selected channels)' }, 'Group'),
      sel([['', 'All'], ['-', 'Unsorted'], ...gs.map(g => [g, g])], S.p.rackGroup || '', v => { S.p.rackGroup = v; commit(''); build(); }, { class: 'grpsel' })];
  }
  function build() {
    toolbar(); lastPat = S.pat; applySwing();
    const steps = stepsNow();
    if (!S.p.channels.length) { rows.replaceChildren(h('small', { class: 'empty', style: 'padding:18px;display:block' }, 'The rack is empty. Use “+ Channel” to add a synth or sampler, or drop audio files here.')); return; }
    const list = shown();
    rows.replaceChildren(...list.map(c => row(c, steps)), ...(list.length ? [] : [h('small', { class: 'empty', style: 'padding:18px;display:block' }, 'No channels in this group.')]));
    refresh();
  }
  // Overview rows: the notes as a miniature piano roll, or the channel's waveform at every hit.
  function drawView(r, now = playTick) {
    const cv = r.lastChild, c = chOf(r.dataset.id), p = pat(), g = cv.getContext('2d'), W = cv.width, H = cv.height, L = p.bars * BAR, ns = (p.notes[c.id] || []).filter(n => n.t < L);
    g.clearRect(0, 0, W, H); g.fillStyle = cssVar('--bg3'); g.fillRect(0, 0, W, H);
    for (let t = 0; t < L; t += BEAT) { g.fillStyle = t % BAR === 0 ? 'rgba(128,128,128,.5)' : 'rgba(128,128,128,.2)'; g.fillRect(Math.round(t / L * W), 0, 1, H); }
    g.fillStyle = c.color;
    if (prefs.rackView === 'roll') {
      if (!ns.length) return;
      const lo = Math.min(...ns.map(n => n.n)), span = Math.max(1, Math.max(...ns.map(n => n.n)) - lo);
      for (const n of ns) {
        const x = n.t / L * W, y = 3 + (1 - (n.n - lo) / span) * (H - 10), w = Math.max(2, n.d / L * W - 1), on = now >= n.t && now < n.t + n.d;
        g.globalAlpha = on ? 1 : 0.4 + 0.6 * n.v; g.fillRect(x, y, w, 4);
        if (on) { g.fillStyle = '#fff'; g.globalAlpha = 0.85; g.fillRect(x, y - 1, w, 6); g.globalAlpha = 1; g.fillStyle = c.color; g.fillRect(x + 1, y, Math.max(1, w - 2), 4); }   // sounding now
      }
      g.globalAlpha = 1; return;
    }
    const smp = c.type === 'sample' && samples.get(c.sample), spt = 60 / S.p.bpm / PPQ, shape = !smp && c.synth ? waveShape(c.synth.osc[0].wave, 24) : null;
    const full = smp ? smp.buffer.duration / spt / L * W : 0;   // pixels the whole sample would cover at this tempo
    const one = (x0, wpx, a) => {
      g.globalAlpha = a;
      if (smp) {
        const d = smp.buffer.getChannelData(0), per = d.length / Math.max(1, full), stride = Math.max(1, Math.floor(per / 24));
        for (let x = 0; x < wpx && x * per < d.length; x++) { let m = 0; for (let i = Math.floor(x * per), e = Math.min(d.length, i + per); i < e; i += stride) { const v = Math.abs(d[i]); if (v > m) m = v; } const y = Math.max(1, m * H); g.fillRect(x0 + x, (H - y) / 2, 1, y); }
      } else if (shape) for (let x = 0; x < wpx; x++) { const v = shape[Math.floor(x * 1.5) % 24] * (H / 2 - 3); g.fillRect(x0 + x, H / 2 - Math.max(0, v), 1, Math.max(1, Math.abs(v))); }
    };
    if (!ns.length) { one(0, smp ? Math.min(W, full) : 48, 0.3); g.globalAlpha = 1; return; }
    const sorted = [...ns].sort((a, b) => a.t - b.t);
    sorted.forEach((n, i) => { const x0 = n.t / L * W, next = sorted[i + 1] ? sorted[i + 1].t / L * W : W; one(x0, Math.max(2, Math.min(next - x0 - 1, smp && !c.gate ? W : n.d / L * W)), 0.4 + 0.6 * n.v); });
    g.globalAlpha = 1;
  }
  function refresh() {
    const p = pat();
    phead.style.display = prefs.rackView !== 'steps' && playTick >= 0 ? 'block' : 'none';
    if (prefs.rackView !== 'steps') { for (const r of rows.querySelectorAll('.row')) drawView(r); return; }
    for (const r of rows.querySelectorAll('.row')) {
      const m = new Map();
      for (const n of p.notes[r.dataset.id] || []) { const i = Math.floor(n.t / STEP); m.set(i, Math.max(m.get(i) || 0, n.v)); }
      const bs = r.lastChild.children;
      for (let i = 0; i < bs.length; i++) { const v = m.get(i); bs[i].classList.toggle('on', !!v); if (v) bs[i].style.setProperty('--v', 0.3 + 0.7 * v); }
    }
  }
  function setStep(b, on, v = 0.8) {
    if (b.classList.contains('on') === on) return;
    const id = b.closest('.row').dataset.id, c = chOf(id), i = +b.dataset.i, p = pat();
    if (on) { const n = c.type === 'synth' ? 60 : c.root; notesOf(p, id).push({ id: uid(), t: i * STEP, d: STEP, n, v }); if (!engine.playing) engine.preview(c, n, v, 0.2); }
    else p.notes[id] = notesOf(p, id).filter(n => Math.floor(n.t / STEP) !== i);
    b.classList.toggle('on', on); b.style.setProperty('--v', 0.3 + 0.7 * v);
  }
  let paint = null;
  rows.addEventListener('pointerdown', e => {
    const b = e.target.closest('.st');
    if (!b || (e.button !== 0 && e.button !== 2) || e.pointerType !== 'mouse') return;
    paint = { on: e.button === 2 ? false : !b.classList.contains('on'), row: b.closest('.row') }; setStep(b, paint.on);   // right button: remove (drag to remove several)
  });
  rows.addEventListener('pointermove', e => {
    if (!paint) return;
    const t = document.elementFromPoint(e.clientX, e.clientY), b = t && t.closest && t.closest('.st');
    if (b && b.closest('.row') === paint.row) setStep(b, paint.on);
  });
  addEventListener('pointerup', () => { if (paint) { paint = null; commit('notes'); } });
  rows.addEventListener('click', e => { const b = e.target.closest('.st'); if (b && e.pointerType !== 'mouse') { setStep(b, !b.classList.contains('on')); commit('notes'); } });
  rows.addEventListener('contextmenu', e => { if (e.target.closest('.st')) e.preventDefault(); });   // right-click removes steps (above); velocity is set in the Piano Roll

  // Drop audio from the library, a sample folder or the computer: onto a sampler's name to replace its sound, anywhere else for a new channel.
  el.addEventListener('dragover', e => e.preventDefault());
  el.addEventListener('drop', async e => {
    e.preventDefault();
    const r = e.target.closest && e.target.closest('.cname') && e.target.closest('.row'), onto = r && chOf(r.dataset.id);
    for (const id of await dropped(e)) {
      const smp = samples.get(id); if (!smp) continue;
      if (onto && onto.type === 'sample') { onto.sample = id; commit(); toast(`“${onto.name}” now plays ${smp.name}`); }
      else addCh(newChannel('sample', { name: smp.name.replace(/ \(.*/, '').slice(0, 18), sample: id, gate: smp.buffer.duration > 1.2, rel: 0.25 }));
    }
  });

  function rowMenu(c, x, y) {
    const p = pat(), L = p.bars * BAR, steps = stepsNow();
    const put = fn => { p.notes[c.id] = []; for (let i = 0; i < steps; i++) { const v = fn(i); if (v) p.notes[c.id].push({ id: uid(), t: i * STEP, d: STEP, n: c.type === 'synth' ? 60 : c.root, v: v === true ? 0.8 : v }); } commit('notes'); };
    const shift = d => { notesOf(p, c.id).forEach(n => { n.t = (n.t + d + L) % L; }); commit('notes'); };
    menu(x, y, [
      { head: c.name },
      { label: 'Rename…', fn: async () => { const v = await ask('Channel name', c.name); if (v) { c.name = v; commit(); } } },
      { label: 'Next colour', fn: () => { c.color = COLORS[(COLORS.indexOf(c.color) + 1) % COLORS.length]; commit(); } },
      { label: 'Group' + (c.group ? ' (' + c.group + ')' : '') + '…', hint: 'Alt+G', fn: async () => { const ids = selectedChannels(), chs = ids.has(c.id) ? S.p.channels.filter(z => ids.has(z.id)) : [c]; const v = await ask('Group name (empty to ungroup)', c.group || 'Drums'); if (v == null) return; chs.forEach(z => { if (v) z.group = v; else delete z.group; }); commit(); } },
      { label: 'Edit instrument', fn: () => { pick(c.id); bus.emit('goto', { to: 'inst', from: 'rack' }); } },
      '-',
      { label: 'Fill every 2 steps', fn: () => put(i => i % 2 === 0) }, { label: 'Fill every 4 steps', fn: () => put(i => i % 4 === 0) },
      { label: 'Euclidean fill…', fn: async () => { const k = +(await ask(`Hits to spread over ${steps} steps`, '5')); if (k > 0) put(i => (i * k) % steps < k); } },
      { label: 'Randomise', fn: () => put(i => (Math.random() < (i % 4 ? 0.22 : 0.5) ? 0.5 + Math.random() * 0.5 : 0)) },
      { label: 'Shift left', fn: () => shift(-STEP) }, { label: 'Shift right', fn: () => shift(STEP) },
      { label: 'Clear steps', fn: () => put(() => 0) },
      '-',
      { label: 'Move this channel’s notes to a new pattern', disabled: !(p.notes[c.id] || []).length, fn: () => {
        const n = { id: uid(), name: `${p.name} · ${c.name}`, bars: p.bars, notes: { [c.id]: p.notes[c.id] } };
        delete p.notes[c.id]; S.p.patterns.push(n); commit(); toast(`“${n.name}” created`);
      } },
      { label: 'Duplicate channel', fn: () => {
        const n = JSON.parse(JSON.stringify(c)); n.id = uid(); n.name = c.name + ' 2';
        S.p.channels.splice(S.p.channels.indexOf(c) + 1, 0, n);
        S.p.patterns.forEach(q => { if (q.notes[c.id]) q.notes[n.id] = q.notes[c.id].map(z => ({ ...z, id: uid() })); });
        S.ch = n.id; commit();
      } },
      { label: 'Move up', fn: () => { const a = S.p.channels, i = a.indexOf(c); if (i > 0) { a.splice(i, 1); a.splice(i - 1, 0, c); commit(); } } },
      { label: 'Move down', fn: () => { const a = S.p.channels, i = a.indexOf(c); if (i < a.length - 1) { a.splice(i, 1); a.splice(i + 1, 0, c); commit(); } } },
      { label: 'Delete channel', disabled: S.p.channels.length < 2, fn: () => {
        S.p.channels = S.p.channels.filter(z => z !== c); S.p.patterns.forEach(q => delete q.notes[c.id]);
        S.p.song.auto = S.p.song.auto.filter(l => !l.target.startsWith('ch:' + c.id + ':'));
        if (S.ch === c.id) S.ch = S.p.channels[0].id; commit();
      } },
    ]);
  }

  // Playhead for the Notes and Wave row views: one line over all rows; rows redraw each step so sounding notes light up.
  let last = -1;
  bus.on('pos', t => {
    if (!el.isConnected) return;
    const i = t < 0 || S.mode === 'song' ? -1 : Math.floor(t / STEP);
    if (prefs.rackView !== 'steps') {
      const cv = rows.querySelector('.rview'), L = pat().bars * BAR;
      if (i < 0 || !cv) { phead.style.display = 'none'; if (playTick !== -1) { playTick = -1; refresh(); } }
      else {
        if (phead.parentNode !== rows) rows.append(phead);
        const rr = rows.getBoundingClientRect(), cr = cv.getBoundingClientRect(), tt = t % L;
        Object.assign(phead.style, { display: 'block', left: cr.left - rr.left + rows.scrollLeft + tt / L * cr.width + 'px', height: rows.scrollHeight - 12 + 'px' });
        if (i !== last) { playTick = tt; refresh(); }
      }
    }
    if (i === last) return;
    last = i;
    rows.querySelectorAll('.st.now').forEach(b => b.classList.remove('now'));
    if (i >= 0) rows.querySelectorAll(`.st[data-i="${i}"]`).forEach(b => b.classList.add('now'));
  });
  bus.on('project', build);
  bus.on('notes', refresh);
  bus.on('samples', () => { if (prefs.rackView === 'wave' && el.isConnected) refresh(); });
  bus.on('sel', () => {
    if (lastPat !== S.pat) { lastPat = S.pat; build(); return; }
    for (const r of rows.children) r.classList.toggle('sel', r.dataset.id === S.ch);
  });
  build();
  // Rack hotkeys: route to free buses, clone, group, delete - for the selected channels.
  el.addEventListener('keydown', e => {
    const id = actionFor(e, 'rack'); if (!id) return;
    const P = S.p, ids = selectedChannels(), chs = P.channels.filter(c => ids.has(c.id)); if (!chs.length) return;
    e.preventDefault(); e.stopPropagation();
    if (id === 'rack.route') {
      for (const c of chs) { const b = addBus(c.name.slice(0, 16)); c.bus = b.id; }
      commit(); toast(`${chs.length} channel${chs.length === 1 ? '' : 's'} now each have their own mixer bus`);
    } else if (id === 'rack.clone') {
      for (const c of chs) { const n = JSON.parse(JSON.stringify(c)); n.id = uid(); n.name = c.name + ' 2'; P.channels.splice(P.channels.indexOf(c) + 1, 0, n); P.patterns.forEach(q => { if (q.notes[c.id]) q.notes[n.id] = q.notes[c.id].map(z => ({ ...z, id: uid() })); }); }
      commit(); toast(`Cloned ${chs.length} channel${chs.length === 1 ? '' : 's'}`);
    } else if (id === 'rack.group') {
      ask('Group name for the selected channels', chs[0].group || 'Drums').then(v => { if (v == null) return; chs.forEach(c => { if (v) c.group = v; else delete c.group; }); commit(); toast(v ? `Grouped as “${v}” - choose it under Group to show only these` : 'Ungrouped'); });
    } else if (id === 'rack.del') {
      if (chs.length >= P.channels.length) return toast('Keep at least one channel');
      P.channels = P.channels.filter(c => !ids.has(c.id)); P.patterns.forEach(q => ids.forEach(i => delete q.notes[i]));
      P.song.auto = P.song.auto.filter(l => ![...ids].some(i => l.target.startsWith('ch:' + i + ':') || l.target.startsWith('plug:' + i + ':')));
      S.chSel = []; if (ids.has(S.ch)) S.ch = P.channels[0].id; commit(); toast('Channels deleted');
    }
  });
  return { el, show: () => { if (prefs.rackView !== 'steps') refresh(); } };
}
