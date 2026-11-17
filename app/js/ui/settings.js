// Appearance, layout, audio and input preferences.
import { h, sel, btn, bus, toast, ask, pickFile, STEP } from '../util.js';
import { S, prefs, savePrefs, setProject, demoProject, blankProject, commit, backups, saveNow, changeSig, stashLast, templates, saveTemplate, deleteTemplate, fromTemplate } from '../state.js';
import { midi } from '../midi.js';
import { engine } from '../audio/engine.js';
import { FX } from '../audio/fx.js';
import { askServer, setPlugServer, SHARE_PORT } from '../audio/bridge.js';
import { folders, addFolder, openFolder, removeFolder, canKeepFolders } from '../samples.js';
import { ACTIONS, SCOPES, MOUSE, comboOf, keyOf, rebind, clash } from '../keys.js';

export const THEMES = {
  ember: { name: 'Ember', vars: { bg: '#15131a', bg2: '#1e1b25', bg3: '#2a2633', line: '#3a3545', text: '#ece8f1', dim: '#9089a1', accent: '#ff7a59', accent2: '#ffc857' } },
  tidal: { name: 'Tidal', vars: { bg: '#0c1618', bg2: '#122023', bg3: '#1a2d31', line: '#284349', text: '#dff3f1', dim: '#7fa3a3', accent: '#3ddbc0', accent2: '#7cc4ff' } },
  orchid: { name: 'Orchid', vars: { bg: '#17121c', bg2: '#211a29', bg3: '#2e2439', line: '#45365a', text: '#f1e9f7', dim: '#a290b5', accent: '#d77bff', accent2: '#ff8fb8' } },
  moss: { name: 'Moss', vars: { bg: '#11150f', bg2: '#191f16', bg3: '#242c1f', line: '#36422e', text: '#e9f0df', dim: '#93a283', accent: '#a6e05a', accent2: '#f2d95c' } },
  slate: { name: 'Slate', vars: { bg: '#16181c', bg2: '#1e2126', bg3: '#2a2e35', line: '#3b414b', text: '#e7eaef', dim: '#8b93a1', accent: '#6ea8ff', accent2: '#ffb86b' } },
  mono: { name: 'Mono', vars: { bg: '#0d0d0d', bg2: '#161616', bg3: '#222222', line: '#353535', text: '#f0f0f0', dim: '#8c8c8c', accent: '#f0f0f0', accent2: '#9a9a9a' } },
  paper: { name: 'Paper', light: true, vars: { bg: '#e9e2d6', bg2: '#f7f2e9', bg3: '#e2d9ca', line: '#cbbfab', text: '#2b2620', dim: '#7d7364', accent: '#d9480f', accent2: '#1c6fd1' } },
  frost: { name: 'Frost', light: true, vars: { bg: '#dde4ea', bg2: '#f3f6f9', bg3: '#d5dde5', line: '#b9c5d1', text: '#1d2530', dim: '#66758a', accent: '#2b6fe0', accent2: '#e0542b' } },
};
const lum = hex => { const n = parseInt(hex.slice(1), 16); return (0.299 * (n >> 16 & 255) + 0.587 * (n >> 8 & 255) + 0.114 * (n & 255)) / 255; };

// Background photo: shrunk to screen size, stored on this device, shown behind the (slightly see-through) panels.
const bgGet = () => { try { return localStorage.getItem('lw.bg') || ''; } catch { return ''; } };
async function bgSet(file) {
  const bmp = await createImageBitmap(file), k = Math.min(1, 1920 / Math.max(bmp.width, bmp.height)), cv = document.createElement('canvas');
  cv.width = Math.round(bmp.width * k); cv.height = Math.round(bmp.height * k); cv.getContext('2d').drawImage(bmp, 0, 0, cv.width, cv.height);
  localStorage.setItem('lw.bg', cv.toDataURL('image/jpeg', 0.82));
}

export function applyPrefs() {
  const t = THEMES[prefs.theme] || THEMES.slate, root = document.documentElement, r = root.style;
  for (const k in t.vars) r.setProperty('--' + k, t.vars[k]);
  const acc = /^#[0-9a-f]{6}$/i.test(prefs.accent) ? prefs.accent : t.vars.accent;
  r.setProperty('--accent', acc); r.setProperty('--on-accent', lum(acc) > 0.55 ? '#16120f' : '#ffffff');
  r.setProperty('--r', prefs.radius + 'px'); r.setProperty('--u', prefs.scale); r.setProperty('--gridk', prefs.gridContrast);
  Object.assign(root.dataset, { density: prefs.density, steps: prefs.stepShape, motion: prefs.motion ? 'on' : 'off', glow: prefs.glow ? 'on' : 'off', light: t.light ? '1' : '0' });
  const bg = bgGet(); root.dataset.bg = bg ? 'on' : 'off'; r.setProperty('--bgimg', bg ? 'url(' + bg + ')' : 'none'); r.setProperty('--bgdim', prefs.bgDim);
  const m = document.querySelector('meta[name=theme-color]'); if (m) m.content = t.vars.bg2;
  bus.emit('theme');
}

export default function Settings() {
  const el = h('div', { class: 'view settings' });
  let tab = 'theme', devs = [];
  const up = (k, v, layout) => { prefs[k] = v; savePrefs(); applyPrefs(); if (layout) bus.emit('layout'); };
  const row = (label, ctl, hint) => h('div', { class: 'srow' }, h('div', null, h('b', null, label), hint && h('small', null, hint)), ctl);
  const range = (k, min, max, step) => h('input', { type: 'range', min, max, step, value: prefs[k], oninput: e => up(k, +e.target.value) });
  const check = (k, layout) => h('input', { type: 'checkbox', checked: !!prefs[k], onchange: e => up(k, e.target.checked, layout) });
  const sec = (title, ...rows) => h('section', { class: 'card' }, h('h4', null, title), rows);
  const restart = () => toast('Applies after restarting the app');
  const ms = v => (v * 1000).toFixed(1) + ' ms';
  const scan = async () => { try { devs = await navigator.mediaDevices.enumerateDevices(); } catch { devs = []; } if (el.isConnected && tab === 'audio') build(); };

  const pages = {
    theme: () => [
      sec('Look',
        row('Theme', h('div', { class: 'swatches' }, Object.entries(THEMES).map(([k, t]) => h('button', {
          class: 'swatch' + (prefs.theme === k ? ' on' : ''), title: t.name, style: `background:linear-gradient(135deg,${t.vars.bg2} 55%,${t.vars.accent} 55% 80%,${t.vars.accent2} 80%)`,
          onclick: () => { prefs.accent = ''; up('theme', k); build(); },
        }, h('span', null, t.name))))),
        row('Accent colour', h('div', { class: 'hrow' }, h('input', { type: 'color', value: /^#[0-9a-f]{6}$/i.test(prefs.accent) ? prefs.accent : THEMES[prefs.theme].vars.accent, oninput: e => up('accent', e.target.value) }), btn('Reset', () => { up('accent', ''); build(); })), 'Overrides the theme accent'),
        row('Background photo', h('div', { class: 'hrow' },
          btn('Upload photo…', async () => { const [f] = await pickFile('image/*'); if (!f) return; try { await bgSet(f); applyPrefs(); build(); toast('Background set'); } catch { toast('That image could not be used (too large or not an image)'); } }, 'primary'),
          bgGet() && btn('Remove', () => { try { localStorage.removeItem('lw.bg'); } catch { /* blocked */ } applyPrefs(); build(); })), 'Shown behind the panels; stays on this device'),
        bgGet() && row('Photo dimming', range('bgDim', 0.1, 0.95, 0.05), 'Higher = darker photo, easier reading'),
        row('Corners', range('radius', 0, 18, 1)),
        row('Interface size', range('scale', 0.85, 1.3, 0.05)),
        row('Density', sel([['compact', 'Compact'], ['cozy', 'Cozy'], ['roomy', 'Roomy']], prefs.density, v => up('density', v))),
        row('Step buttons', sel([['lozenge', 'Lozenge'], ['square', 'Square'], ['dot', 'Dot']], prefs.stepShape, v => up('stepShape', v))),
        row('Note colours', sel([['channel', 'Channel colour'], ['accent', 'Accent'], ['velocity', 'Velocity heat'], ['pitch', 'Pitch class']], prefs.noteColor, v => up('noteColor', v))),
        row('Grid contrast', range('gridContrast', 0.3, 2, 0.1)),
        row('Glow & gradients', check('glow')),
        row('Animations', check('motion'))),
    ],
    general: () => [
      sec('Layout',
        row('Workspace', sel([['single', 'One deck'], ['stack', 'Two decks, stacked'], ['side', 'Two decks, side by side'], ['tri', 'Three decks (one wide, two below)'], ['quad', 'Four decks (2 × 2)']], prefs.layout, v => up('layout', v, true)), 'Any view can live in any deck. Phones always use one deck.'),
        row('Floating deck', check('float', true), 'An extra deck in a window you can drag by its tab bar and resize from its corner'),
        row('On-screen keyboard', check('keys', true))),
      sec('Editing',
        row('Song snap', sel([[1, 'Bar'], [0.5, '1/2 bar'], [0.25, 'Beat'], [0.125, '1/2 beat'], [0.0625, 'Step'], [0, 'Off']], prefs.songSnap, v => { up('songSnap', +v); bus.emit('project'); }), 'Grid for placing and moving clips in the Song view. Alt while dragging ignores it.'),
        row('Ghost notes', check('ghost'), 'Show other channels faintly in the piano roll'),
        row('Lock notes to scale', check('scaleLock'))),
      sec('Shortcuts', row('Keyboard and mouse shortcuts', btn('Open Hotkeys', () => { tab = 'hotkeys'; build(); }), 'See every shortcut and set your own keys')),
    ],
    audio: () => {
      const ctx = engine.ctx, outs = devs.filter(d => d.kind === 'audiooutput' && d.deviceId !== 'default' && d.deviceId !== 'communications'), ins = devs.filter(d => d.kind === 'audioinput' && d.deviceId !== 'default' && d.deviceId !== 'communications');
      const lat = (ctx.baseLatency || 0) + (ctx.outputLatency || 0);
      return [
        sec('Input / output',
          row('Output device', ctx.setSinkId
            ? sel([['', 'System default'], ...outs.map((d, i) => [d.deviceId, d.label || 'Output ' + (i + 1)])], prefs.sinkId, async v => { if (await engine.setSink(v)) { up('sinkId', v); toast('Output changed'); } else toast('That output could not be opened'); build(); })
            : h('small', null, 'This device does not let apps choose an output; it follows the system output.'),
            'Speakers, headphones, an audio interface, or a JACK / PipeWire sink'),
          row('Input device', sel([['', 'System default'], ...ins.map((d, i) => [d.deviceId, d.label || 'Input ' + (i + 1)])], prefs.inputId || '', async v => {
            up('inputId', v);
            for (const b of S.p.buses) if (b.arm && b.input === 'default') { engine.disarm(b); await engine.arm(b); }   // re-open armed buses on the new input
            toast('Recording input changed');
          }), 'Microphone or line input used by buses set to “Default input” (Mixer > IN ▾). Press “Show device names” if the list shows no names.'),
          row('Device list', h('div', { class: 'hrow' }, btn('Rescan', scan), btn('Show device names', async () => { try { (await navigator.mediaDevices.getUserMedia({ audio: true })).getTracks().forEach(t => t.stop()); } catch { toast('Permission was denied'); } scan(); }, '', 'Asks for audio permission once so devices can be listed by name')), 'Recording inputs are chosen per bus in the Mixer (IN ▾)'),
          row('Sample rate', sel([[0, 'Match device'], [44100, '44100 Hz'], [48000, '48000 Hz'], [88200, '88200 Hz'], [96000, '96000 Hz']], prefs.sampleRate, v => { up('sampleRate', +v); restart(); })),
          row('Buffer / latency', sel([['interactive', 'Smallest the device allows'], [0.01, '10 ms'], [0.02, '20 ms'], [0.04, '40 ms'], ['balanced', 'Balanced'], [0.1, '100 ms (safe)'], ['playback', 'Power saving']], prefs.latency, v => { up('latency', isNaN(+v) ? v : +v); restart(); }), 'Smaller = tighter live playing; larger = fewer crackles on slow devices'),
          row('Status', h('small', { class: 'status' }, `${ctx.state === 'running' ? 'Open' : 'Suspended'} at ${ctx.sampleRate} Hz · buffer ${Math.round((ctx.baseLatency || 0) * ctx.sampleRate)} smp (${ms(ctx.baseLatency || 0)}) · output ${ms(ctx.outputLatency || 0)} · total ${ms(lat)}`)),
          row('Recording offset', h('div', { class: 'hrow' }, h('input', { type: 'range', min: -100, max: 200, step: 1, value: prefs.recOffset, oninput: e => { up('recOffset', +e.target.value); e.target.nextSibling.textContent = prefs.recOffset + ' ms'; } }), h('small', null, prefs.recOffset + ' ms')), 'Shifts recorded takes earlier to line up with the beat; raise it if takes land late'),
          row('Restart audio', btn('Reload app', () => location.reload()), 'Applies sample rate and buffer changes')),
        sec('Quality',
          row('Bit depth', sel([[16, '16-bit (CD quality, smallest files)'], [24, '24-bit (studio standard)'], [32, '32-bit float (highest, largest files)']], prefs.bitDepth, v => up('bitDepth', +v)), 'Used for WAV export and for storing recordings and designed sounds'),
          row('Export sample rate', sel([[44100, '44.1 kHz'], [48000, '48 kHz'], [88200, '88.2 kHz'], [96000, '96 kHz']], prefs.exportRate, v => up('exportRate', +v)), 'Rate of exported WAV files. Recording uses the device rate shown under Status; raise Sample rate above to record higher.'),
          row('Compressed format', sel([['mp3', 'MP3 (plays everywhere)'], ['opus', 'Ogg Opus (smaller at the same quality)']], prefs.compFormat || 'mp3', v => up('compFormat', v)), 'What Ctrl+Shift+R exports; the File menu offers both'),
          row('MP3 bitrate', sel([[128, '128 kbps'], [192, '192 kbps'], [256, '256 kbps'], [320, '320 kbps (highest)']], prefs.mp3Kbps || 320, v => up('mp3Kbps', +v)), 'MP3 is 44.1 or 48 kHz; other export rates are resampled'),
          row('Opus bitrate', sel([[96, '96 kbps'], [128, '128 kbps'], [192, '192 kbps'], [256, '256 kbps']], prefs.opusKbps || 192, v => up('opusKbps', +v))),
          h('small', null, 'WAV is uncompressed, so it has no bitrate: quality is set by bit depth and sample rate. 24-bit / 48 kHz is about 2,304 kbps in stereo. MP3 encoding uses LAME (lame.sourceforge.net, LGPL).')),
        sec('Drivers', h('small', null, 'Undae Sonicae uses the Linux audio system: PipeWire, PulseAudio, ALSA or JACK. For the lowest latency, run PipeWire or JACK with a small buffer (quantum) and choose its device under Output device. Class-compliant USB audio interfaces work without extra drivers.')),
        sec('CPU',
          row('Scheduler look-ahead', sel([[0.06, '60 ms (tight)'], [0.12, '120 ms'], [0.25, '250 ms (safe)']], prefs.lookahead, v => up('lookahead', +v)), 'Raise this if notes drop out on slow devices'),
          row('Max synth voices', sel([4, 8, 12, 16, 24, 32], prefs.maxVoices, v => up('maxVoices', +v))),
          row('Meter refresh', sel([[0, 'Off'], [15, '15 fps'], [30, '30 fps'], [60, '60 fps']], prefs.meterFps, v => up('meterFps', +v))),
          row('Multi-threaded processing', (() => {
            const box = h('input', { type: 'checkbox', disabled: true, onchange: async e => { const r = await fetch('engine.json', { method: 'PUT', body: JSON.stringify({ threads: e.target.checked }) }).then(x => x.json()).catch(() => ({})); toast(r.ok ? 'Takes effect when you restart Undae Sonicae' : 'Could not save this setting'); } });
            fetch('engine.json', { cache: 'no-store' }).then(r => r.json()).then(o => { box.checked = o.threads !== false; box.disabled = false; }).catch(() => { box.title = 'Only in the desktop app'; });
            return box;
          })(), 'Desktop app. On: the audio thread runs at real-time priority and plug-ins process in parallel, each on its own thread. Off: plug-ins take turns on one thread, which uses less CPU. Applies after a restart.'),
          row('Fewer dropouts', h('small', null, 'If you hear crackles, raise “Buffer / latency” above. Plug-ins grow their own buffer after a dropout and remember the size for next time.'))),
      ];
    },
    midi: () => [
      sec('Input devices',
        midi.ok
          ? (midi.inputs.length ? h('div', { class: 'devlist' }, midi.inputs.map(i => { const key = i.name || i.id; return h('label', null, h('input', { type: 'checkbox', checked: !prefs.midiOff[key], onchange: e => { if (e.target.checked) delete prefs.midiOff[key]; else prefs.midiOff[key] = true; savePrefs(); } }), h('span', null, i.name || 'MIDI input'), h('small', null, i.manufacturer || '')); }))
            : h('small', null, 'MIDI is on. No devices found yet - plug one in (USB-OTG on phones).'))
          : h('small', null, midi.err || 'Not connected.'),
        row('Connection', h('div', { class: 'hrow' }, btn(midi.ok ? 'Rescan' : 'Enable MIDI', () => midi.init().then(build), 'primary'), h('small', null, midi.last ? 'Last message: ' + midi.last : '')), 'Ticked devices play the selected channel')),
      sec('Playing',
        row('Listen to channel', sel([[0, 'Omni (all)'], ...Array.from({ length: 16 }, (_, i) => [i + 1, 'Channel ' + (i + 1)])], prefs.midiCh, v => up('midiCh', +v))),
        row('Velocity response', sel([['soft', 'Soft (light touch is louder)'], ['linear', 'Linear'], ['hard', 'Hard (needs a firmer touch)'], ['fixed', 'Fixed']], prefs.velCurve, v => up('velCurve', v))),
        row('Foot pedal sustains notes', check('pedal')),
        row('Record quantize', sel([[0, 'Off'], [STEP * 2, '1/8'], [STEP, '1/16'], [STEP / 2, '1/32']], prefs.recQuant, v => up('recQuant', +v))),
        row('Typing keyboard', check('typing'), 'Z-M and Q-P rows play notes'),
        row('Typing octave', sel([1, 2, 3, 4, 5], prefs.octave, v => up('octave', +v)))),
    ],
    file: () => {
      const usage = h('small', null, '…'), list = backups();
      if (navigator.storage && navigator.storage.estimate) navigator.storage.estimate().then(e => { usage.textContent = `${(e.usage / 1048576).toFixed(1)} MB used of ${(e.quota / 1073741824).toFixed(1)} GB available`; });
      return [
        sec('Backup',
          row('Autosave', sel([[0.7, 'After every change'], [30, 'Every 30 seconds'], [300, 'Every 5 minutes'], [0, 'Off']], prefs.autosave, v => up('autosave', +v)), 'Saved on this device; use Save project file for a copy you can move'),
          row('Keep timed backups', check('backups'), 'Up to 5 snapshots, at most one every 10 minutes'),
          row('Save now', btn('Save', () => { saveNow(); toast('Saved'); }))),
        sec('Restore a backup', list.length ? h('div', { class: 'devlist' }, list.map(b => h('label', null, h('span', null, b.name), h('small', null, new Date(b.at).toLocaleString()), btn('Restore', () => { try { setProject(JSON.parse(b.data)); toast('Backup restored'); } catch { toast('That backup is damaged'); } }, 'sm')))) : h('small', null, 'No backups yet.')),
        sec('Sample folders',
          h('small', null, 'Folders listed here appear in the Library, where you can audition files and drag them onto the Rack or a Song track without importing the whole folder.'),
          h('div', { class: 'devlist' }, folders.filter(f => f.kind === 'samples').map(f => h('label', null, h('span', null, '📁 ' + f.name), h('small', null, f.ready ? f.files.length + ' audio files' : 'locked'), !f.ready && btn('Allow', () => openFolder(f), 'sm'), btn('✕', () => removeFolder(f), 'sm', 'Forget this folder')))),
          row('Add a location', btn('Add sample folder…', () => addFolder('samples'), 'primary'), canKeepFolders() ? 'Remembered between sessions; the system may ask you to allow access again after a restart' : 'On this device folders cannot be remembered: pick files or a folder each session')),
        sec('Storage', row('Uploaded samples & recordings', usage, 'Kept in this app’s own storage on the device')),
      ];
    },
    project: () => {
      const P = S.p;
      return [
        sec('Current project',
          row('Name', h('input', { type: 'text', value: P.name, onchange: e => { P.name = e.target.value || 'Untitled'; commit(); }, onkeydown: e => e.stopPropagation() })),
          row('Tempo', h('small', null, P.bpm + ' BPM')),
          row('Timebase', h('small', null, '96 PPQ')), row('Time signature', h('div', { class: 'hrow' }, sel([2, 3, 4, 5, 6, 7, 9, 11, 12, 13, 15], P.sig.n, v => changeSig(+v, P.sig.d)), h('span', null, '/'), sel([2, 4, 8, 16], P.sig.d, v => changeSig(P.sig.n, +v))), 'Also on the top bar next to the tempo'),
          row('Swing grid', sel([[16, '1/16 notes'], [8, '1/8 notes']], P.swingDiv || 16, v => { P.swingDiv = +v; commit(); })),
          row('Contents', h('small', null, `${P.channels.length} channels · ${P.patterns.length} patterns · ${P.buses.length} buses · ${P.song.clips.length} pattern clips · ${P.song.audio.length} audio clips`))),
        sec('Starting up',
          row('When the program opens', sel([['blank', 'Start with a blank project'], ['last', 'Reopen where I left off'], ...Object.keys(templates()).map(n => ['tpl:' + n, 'Start from template: ' + n])], prefs.startup, v => up('startup', v)), 'A blank project has an empty rack and only the master bus. Your previous session is kept under the project menu > Reopen last session.'),
          row('Demo song', btn('Load demo', () => { stashLast(S.p); setProject(demoProject()); toast('Demo loaded'); }), 'A small finished example with drums, keys, bass and lead'),
          row('Empty project', btn('Blank project', () => { stashLast(S.p); setProject(blankProject()); })),
          row('Templates', h('div', { class: 'hrow' }, btn('Save this project as a template…', async () => { const v = await ask('Template name', S.p.name === 'Untitled' ? 'My template' : S.p.name); if (v) { toast(saveTemplate(v, S.p) ? `Template “${v}” saved` : 'Not enough storage for this template'); build(); } })), 'Your own starting points: channels, mixer, effects and patterns. Choose one above to open it at start-up.'),
          ...Object.keys(templates()).map(n => row(n, h('div', { class: 'hrow' }, btn('New from it', () => { stashLast(S.p); setProject(fromTemplate(n)); toast('New project from “' + n + '”'); }, 'sm'), btn('Update', () => { saveTemplate(n, S.p); toast(`“${n}” now matches this project`); }, 'sm', 'Replace the template with the current project'), btn('Delete', () => { deleteTemplate(n); build(); }, 'sm'))))),
      ];
    },
  };
  // --- hotkeys: every shortcut in one list; click a key to press a new one -------------
  let listening = null;
  function capture(id, button) {
    if (listening) listening();
    button.textContent = 'Press keys…'; button.classList.add('on');
    const onKey = e => {
      e.preventDefault(); e.stopPropagation();
      if (e.key === 'Escape') { listening(); build(); return; }
      const c = comboOf(e); if (!c) return;
      const other = clash(id, c);
      rebind(id, c); listening(); build();
      toast(other ? `${c} is also used by “${other}” - change one of them` : `Set to ${c}`);
    };
    listening = () => { removeEventListener('keydown', onKey, true); listening = null; };
    addEventListener('keydown', onKey, true);
  }
  pages.hotkeys = () => [
    ...Object.entries(SCOPES).map(([scope, title]) => sec(title, h('div', { class: 'keys-help' }, ACTIONS.filter(a => a[3] === scope).map(([id, label, def]) => h('div', null,
      h('button', { class: 'btn sm keybtn' + (prefs.keymap[id] ? ' changed' : ''), title: 'Click, then press the new key combination (Esc cancels)', onclick: e => capture(id, e.currentTarget) }, keyOf(id)),
      h('span', null, label), prefs.keymap[id] && btn('↺', () => { rebind(id, null); build(); }, 'sm', 'Back to the default: ' + def)))))),
    sec('Mouse (fixed)', h('div', { class: 'keys-help' }, MOUSE.map(([k, d]) => h('div', null, h('kbd', null, k), h('span', null, d))))),
    sec('Reset', row('All shortcuts', btn('Restore defaults', () => { prefs.keymap = {}; savePrefs(); build(); toast('Shortcuts restored'); }), 'Typing-keyboard notes (Z-M and Q-P rows) are set in the MIDI tab')),
  ];
  // --- plug-ins: built-in effects, plus folders of VST plug-ins that can be listed but not loaded ---
  // Plug-in server: use another computer's plug-ins (any device), or share this computer's (desktop app only).
  let shareCfg;   // what /share-config.json said; undefined until asked, null where there is no desktop app
  const loadShare = () => fetch('share-config.json', { cache: 'no-store' }).then(r => (r.ok ? r.json() : null)).catch(() => null).then(c => { shareCfg = c; if (tab === 'plugins') build(); });
  const putShare = o => fetch('share-config.json', { method: 'PUT', body: JSON.stringify(o) }).then(r => r.json()).then(c => { shareCfg = c; build(); }).catch(() => toast('Could not change sharing'));
  function serverSecs() {
    const sv = prefs.plugServer || { addr: '', code: '', on: false };
    const addr = h('input', { class: 'txt', placeholder: 'e.g. 192.168.1.20', value: sv.addr || '', inputMode: 'url', autocapitalize: 'off', spellcheck: false, onkeydown: e => e.stopPropagation() });
    const code = h('input', { class: 'txt', placeholder: '6 digits', value: sv.code || '', inputMode: 'numeric', maxLength: 6, style: 'width:110px', onkeydown: e => e.stopPropagation() });
    const status = h('small', { class: 'status' }, sv.on ? 'Connected to ' + sv.addr : 'Not connected');
    const connect = async () => {
      status.textContent = 'Connecting…';
      const r = await askServer({ addr: addr.value, code: code.value });
      if (!r.ok) { status.textContent = r.error; return; }
      prefs.plugServer = { addr: addr.value.trim(), code: code.value.trim(), on: true }; savePrefs(); setPlugServer(prefs.plugServer); engine.reloadPlugins();
      const what = [r.clap && 'Linux plug-ins', (r.win || r.win32) && 'Windows plug-ins', r.winPending && 'Wine still starting'].filter(Boolean).join(', ');
      toast(`Connected to ${r.name}${what ? ': ' + what : ''}`); build();
    };
    const disconnect = () => { prefs.plugServer = { ...sv, on: false }; savePrefs(); setPlugServer(null); engine.reloadPlugins(); toast('Using this device’s own plug-ins again'); build(); };
    const out = [sec('Use plug-ins from another computer',
      h('small', null, 'Run Undae Sonicae on a Linux computer on the same Wi-Fi, switch on “Share this computer’s plug-ins” there, and enter its address and pairing code here. Its Linux and Windows plug-ins then work in this app: sound goes there and back over the network, adding about 50–100 ms, so it suits playback and mixing better than live playing. Plug-in windows open on that computer’s screen; here you get their knobs.'),
      row('Computer address', addr, 'Shown on the computer under Settings › Plug-ins'), row('Pairing code', code),
      row('', h('div', { class: 'hrow' }, sv.on ? btn('Disconnect', disconnect) : btn('Connect', connect, 'primary'), sv.on && btn('Reconnect', connect)), ''), row('Status', status))];
    if (shareCfg === undefined) { loadShare(); return out; }
    if (!shareCfg) return out;
    const c = shareCfg, port = h('input', { class: 'txt', type: 'number', min: 1024, max: 65535, value: c.port, style: 'width:110px', onkeydown: e => e.stopPropagation(), onchange: e => putShare({ port: +e.target.value }) });
    const toggle = h('input', { type: 'checkbox', checked: c.on, onchange: e => putShare({ on: e.target.checked }) });
    out.unshift(sec('Share this computer’s plug-ins',
      h('small', null, 'Lets the Android app (or another computer) on the same network use the plug-ins installed here. Anyone who wants to connect needs the pairing code below.'),
      row('Share on the network', toggle, c.on ? (c.running ? 'On' + (c.clients ? ` · ${c.clients} connection${c.clients === 1 ? '' : 's'} now` : '') : 'Not running: ' + (c.error || 'unknown problem')) : 'Off'),
      c.on && c.running && row('Address to enter', h('b', null, c.addresses.length ? c.addresses.map(a => a + (c.port === SHARE_PORT ? '' : ':' + c.port)).join('  or  ') : 'no network found'), 'On the other device: Settings › Plug-ins › Computer address'),
      row('Pairing code', h('div', { class: 'hrow' }, h('b', { style: 'font-size:1.4em;letter-spacing:.12em' }, c.code), btn('New code', () => putShare({ newCode: true }), 'sm', 'Devices using the old code are refused from now on')), ''),
      row('Port', port, 'TCP port to listen on (' + SHARE_PORT + ' by default). If the other device cannot connect, allow it in the firewall, e.g. sudo ufw allow ' + c.port + '/tcp'),
      btn('Refresh', loadShare, 'sm')));
    return out;
  }
  pages.plugins = () => {
    const pf = folders.filter(f => f.kind === 'plugins');
    return [
      ...serverSecs(),
      sec('Built-in effects', h('small', null, Object.values(FX).map(d => d.name).join(' · ')), h('small', null, 'Add these on any bus in the Mixer with “+ Add effect”.')),
      sec('CLAP and Windows VST plug-ins', h('small', null, 'The desktop app loads Linux CLAP and VST3 plug-ins and, when Wine is installed, Windows VST2 and VST3 plug-ins, 64-bit and 32-bit. Put .clap files in ~/.clap, Linux .vst3 folders in ~/.vst3 (or /usr/lib/vst3), and Windows .dll or .vst3 files in ~/.vst-win, then add an effect with “Plug-in (CLAP / VST)” on any bus in the Mixer, or an instrument with “+ Channel” > “Plug-in instrument” in the Rack. Parameters appear as knobs and can be automated; “Open window” shows the plug-in’s own window. Plug-in instruments get pitch bend, mod wheel and sustain from your MIDI keyboard. A plug-in effect delays its bus by about 25 ms (35 ms for a Windows plug-in); after an audio dropout the buffer grows by itself, up to about 90 ms, and the slot shows the current delay. Sequenced notes of plug-in instruments are not delayed. Exported audio includes plug-ins.')),
      sec('Plug-in folders',
        h('small', { class: 'warn' }, 'LV2 plug-ins cannot be loaded yet. On Android, plug-ins come from a computer sharing them (above). Folders you add here are scanned and listed so you can see what you own.'),
        pf.map(f => h('div', { class: 'devlist' }, h('label', null, h('span', null, '📁 ' + f.name), h('small', null, f.ready ? f.files.length + ' plug-ins found' : 'locked'), !f.ready && btn('Allow', () => openFolder(f), 'sm'), btn('✕', () => removeFolder(f), 'sm', 'Forget this folder')),
          f.files.slice(0, 200).map(x => h('label', null, h('span', null, x.name), h('small', null, x.name.split('.').pop().toUpperCase() + ' · not loadable'))))),
        row('Scan a folder', btn('Add plug-in folder…', () => addFolder('plugins'), 'primary'), 'Typical places: C:\\Program Files\\Common Files\\VST3, ~/.vst3, /usr/lib/vst3')),
    ];
  };
  const TABS = [['theme', 'Theme'], ['general', 'General'], ['audio', 'Audio'], ['midi', 'MIDI'], ['hotkeys', 'Hotkeys'], ['file', 'File'], ['plugins', 'Plug-ins'], ['project', 'Project']];
  function build() {
    el.replaceChildren(
      h('nav', { class: 'stabs' }, TABS.map(([id, label]) => h('button', { class: id === tab ? 'on' : '', onclick: () => { tab = id; if (id === 'audio') scan(); build(); } }, label))),
      h('div', { class: 'spage' }, pages[tab]()));
  }
  build();
  bus.on('settab', t => { tab = t; build(); });   // F10 / F11 open a tab directly
  bus.on('midi', () => el.isConnected && tab === 'midi' && build());
  bus.on('project', () => el.isConnected && tab === 'project' && build());
  return { el };
}
