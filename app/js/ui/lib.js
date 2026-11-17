// Sample library: built-in 808 kit, stock instruments, uploaded audio, and sample folders on disk.
import { h, btn, bus, toast, pickFile } from '../util.js';
import { S, chan, commit, newChannel } from '../state.js';
import { samples, addFile, removeSample, folders, addFolder, openFolder, removeFolder, drag } from '../samples.js';
import { engine } from '../audio/engine.js';

export default function Library() {
  const list = h('div', { class: 'liblist' }), find = h('input', { class: 'txt', type: 'search', placeholder: 'Filter folder files…', oninput: () => build(), onkeydown: e => e.stopPropagation() });
  const el = h('div', { class: 'view lib' },
    h('div', { class: 'vbar' }, btn('Upload audio…', () => pickFile('audio/*', true).then(add), 'primary'), btn('+ Sample folder', () => addFolder('samples'), '', 'Browse a folder of samples without importing everything'), find,
      h('small', null, 'Tap a name to audition · drag any sound onto the Rack or a Song track')), list);
  async function add(files) {
    for (const f of files) { try { await addFile(f); } catch { toast('Could not decode ' + f.name); } }
  }
  el.addEventListener('dragover', e => e.preventDefault());
  el.addEventListener('drop', e => { e.preventDefault(); if (!drag.id && !drag.file) add([...e.dataTransfer.files]); drag.id = drag.file = null; });

  const toRack = s => {
    const drum = s.id.startsWith('808:');
    const c = newChannel('sample', { name: s.name.replace(/ \(.*/, '').slice(0, 18), sample: s.id, gate: !drum && s.buffer.duration > 1.2, rel: 0.25, root: s.id === 'inst:sub' ? 36 : 60 });
    S.p.channels.push(c); S.ch = c.id; commit(); toast(`Added “${c.name}” to the rack`);
  };
  const toSel = s => {
    const c = chan();
    if (!c || c.type !== 'sample') return toast('Select a sampler channel first');
    c.sample = s.id; commit(); toast(`“${c.name}” now plays ${s.name}`);
  };
  // Folder files are only decoded when you audition or use them.
  const audition = async f => { try { const file = await f.file(); engine.previewBuffer(await engine.ctx.decodeAudioData(await file.arrayBuffer())); } catch { toast('Could not play ' + f.name); } };
  const importFile = async f => { try { return samples.get(await addFile(await f.file())); } catch { toast('Could not decode ' + f.name); return null; } };
  const dragOn = (node, set) => { node.draggable = true; node.addEventListener('dragstart', e => { drag.id = drag.file = null; set(); e.dataTransfer.setData('text/plain', 'sample'); e.dataTransfer.effectAllowed = 'copy'; }); node.addEventListener('dragend', () => setTimeout(() => { drag.id = drag.file = null; }, 50)); return node; };

  function build() {
    const groups = new Map(), q = find.value.trim().toLowerCase();
    for (const s of samples.values()) { const g = s.builtin ? s.group : 'Your audio'; if (!groups.has(g)) groups.set(g, []); groups.get(g).push(s); }
    if (!groups.has('Your audio')) groups.set('Your audio', []);
    list.replaceChildren(
      ...folders.filter(f => f.kind === 'samples').map(f => {
        const hits = q ? f.files.filter(x => x.path.toLowerCase().includes(q)) : f.files;
        return h('section', { class: 'card' }, h('h4', null, '📁 ' + f.name, h('span', { class: 'grow' }), h('small', null, f.ready ? `${hits.length} of ${f.files.length} files` : 'locked'), btn('✕', () => removeFolder(f), 'sm', 'Forget this folder')),
          !f.ready ? btn('Allow access to this folder again', () => openFolder(f), 'primary')
            : hits.length ? [hits.slice(0, 250).map(x => dragOn(h('div', { class: 'librow' },
              h('button', { class: 'lname', title: x.path, onclick: () => audition(x) }, x.name, h('small', null, x.path.includes('/') ? x.path.slice(0, x.path.lastIndexOf('/')) : '')),
              btn('+ Rack', async () => { const s = await importFile(x); s && toRack(s); }, 'sm', 'Import and add as a new channel'), btn('→ Selected', async () => { const s = await importFile(x); s && toSel(s); }, 'sm', 'Import and use for the selected sampler channel')), () => { drag.file = x.file; })),
              hits.length > 250 && h('small', null, `Showing the first 250 - type in the filter box to narrow ${hits.length} files.`)]
              : h('small', null, q ? 'Nothing matches the filter.' : 'No audio files found in this folder.'));
      }),
      ...[...groups].map(([g, items]) => h('section', { class: 'card' }, h('h4', null, g),
        items.length ? items.map(s => dragOn(h('div', { class: 'librow' },
          h('button', { class: 'lname', onclick: () => engine.previewSample(s.id) }, s.name, h('small', null, s.buffer.duration.toFixed(2) + 's')),
          btn('+ Rack', () => toRack(s), 'sm', 'Add as a new channel'), btn('→ Selected', () => toSel(s), 'sm', 'Use as the sound of the selected sampler channel'),
          !s.builtin && btn('✕', () => removeSample(s.id), 'sm', 'Remove from library')), () => { drag.id = s.id; }))
          : h('small', null, 'Nothing here yet. Upload any audio file to use it as a drum hit or a playable instrument.'))));
  }
  bus.on('samples', build); bus.on('folders', build);
  build();
  return { el };
}
