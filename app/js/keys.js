// Keyboard shortcuts: one table of actions with default keys; Settings > Hotkeys can rebind any of them.
import { prefs, savePrefs } from './state.js';

// [id, label, default key, scope]. Scope says where the key is listened for.
export const ACTIONS = [
  // windows (views) and panels
  ['v.song', 'Show Song (playlist)', 'F5', 'global'], ['v.rack', 'Show Rack (channel rack)', 'F6', 'global'], ['v.roll', 'Show Piano Roll', 'F7', 'global'],
  ['picker', 'Add a plug-in instrument (plug-in picker)', 'F8', 'global'], ['v.lib', 'Show Library (sample browser)', 'Alt+F8', 'global'], ['v.mixer', 'Show Mixer', 'F9', 'global'],
  ['midiSet', 'MIDI settings', 'F10', 'global'], ['songInfo', 'Project info', 'F11', 'global'], ['closeAll', 'Close floating panels', 'F12', 'global'], ['closeWin', 'Close the current panel or menu', 'Escape', 'global'],
  ['manual', 'Keyboard shortcuts and help', 'F1', 'global'], ['v.inst', 'Show Instrument', 'Shift+F7', 'global'], ['v.design', 'Show Sound Design', 'F4', 'global'], ['v.lab', 'Show Audio Lab', 'F3', 'global'], ['v.set', 'Show Settings', 'Ctrl+,', 'global'],
  // playback and recording
  ['play', 'Play / stop', 'Space', 'global'], ['pause', 'Play / pause (carry on from where it stopped)', 'Ctrl+Space', 'global'], ['mode', 'Switch pattern / song mode', 'L', 'global'],
  ['rec', 'Record on / off', 'R', 'global'], ['metro', 'Metronome on / off', 'Ctrl+M', 'global'], ['countIn', 'Count-in before recording on / off', 'Ctrl+P', 'global'],
  ['waitInput', 'Wait for a note before recording starts', 'Ctrl+I', 'global'], ['panic', 'Stop all sound (panic)', 'Ctrl+H', 'global'],
  // files
  ['save', 'Save project file', 'Ctrl+S', 'global'], ['saveAs', 'Save as…', 'Ctrl+Shift+S', 'global'], ['saveNew', 'Save a new version', 'Ctrl+N', 'global'], ['open', 'Open project file', 'Ctrl+O', 'global'],
  ['exportWav', 'Export WAV', 'Ctrl+R', 'global'], ['exportComp', 'Export MP3 (or Opus, see Settings › Audio)', 'Ctrl+Shift+R', 'global'], ['exportMidi', 'Export MIDI', 'Ctrl+Shift+M', 'global'],
  ['undo', 'Undo (press again to redo)', 'Ctrl+Z', 'global'], ['undoStep', 'Undo step by step', 'Ctrl+Alt+Z', 'global'], ['redo', 'Redo', 'Ctrl+Shift+Z', 'global'], ['redo2', 'Redo (second key)', 'Ctrl+Y', 'global'],
  ['time', 'Switch time display (bars / minutes)', 'Ctrl+Alt+T', 'global'], ['keys', 'On-screen keyboard', 'Alt+K', 'global'], ['prevPat', 'Previous pattern', 'Alt+ArrowUp', 'global'], ['nextPat', 'Next pattern', 'Alt+ArrowDown', 'global'],
  // Song and Piano Roll tools (letters work when the Song or the Piano Roll has the focus)
  ['song.t.draw', 'Tool: draw', 'P', 'song'], ['song.t.paint', 'Tool: paint', 'B', 'song'], ['song.t.erase', 'Tool: delete', 'D', 'song'], ['song.t.mute', 'Tool: mute', 'T', 'song'],
  ['song.t.slice', 'Tool: slice', 'C', 'song'], ['song.t.select', 'Tool: select', 'E', 'song'], ['song.t.zoom', 'Tool: zoom', 'Z', 'song'], ['song.t.play', 'Tool: play from where you click', 'Y', 'song'], ['song.t.slip', 'Tool: slip (move the content inside a clip)', 'S', 'song'],
  ['song.dup', 'Duplicate clips to the right', 'Ctrl+B', 'song'], ['song.all', 'Select all clips', 'Ctrl+A', 'song'], ['song.source', 'Select all clips from the same source', 'Shift+C', 'song'],
  ['song.group', 'Group selected clips', 'Shift+G', 'song'], ['song.ungroup', 'Ungroup clips', 'Alt+G', 'song'], ['song.insert', 'Insert space at the play marker (one snap length or the time selection)', 'Ctrl+Insert', 'song'],
  ['song.space', 'Delete space at the play marker', 'Ctrl+Delete', 'song'], ['song.quant', 'Quick quantize clip starts', 'Shift+Q', 'song'], ['song.marker', 'Add a time marker', 'Alt+T', 'song'],
  ['song.copy', 'Copy clips', 'Ctrl+C', 'song'], ['song.cut', 'Cut clips', 'Ctrl+X', 'song'], ['song.paste', 'Paste clips at the play marker', 'Ctrl+V', 'song'],
  ['song.del', 'Delete clips', 'Delete', 'song'], ['song.none', 'Clear selection', 'Escape', 'song'],
  ['roll.t.draw', 'Tool: draw', 'P', 'roll'], ['roll.t.paint', 'Tool: paint', 'B', 'roll'], ['roll.t.erase', 'Tool: delete', 'D', 'roll'], ['roll.t.mute', 'Tool: mute notes', 'T', 'roll'],
  ['roll.t.slice', 'Tool: slice', 'C', 'roll'], ['roll.t.select', 'Tool: select', 'E', 'roll'], ['roll.t.zoom', 'Tool: zoom', 'Z', 'roll'], ['roll.t.play', 'Tool: play from where you click', 'Y', 'roll'],
  ['roll.all', 'Select all notes', 'Ctrl+A', 'roll'], ['roll.none', 'Deselect', 'Ctrl+D', 'roll'], ['roll.copy', 'Copy notes', 'Ctrl+C', 'roll'], ['roll.cut', 'Cut notes', 'Ctrl+X', 'roll'], ['roll.paste', 'Paste notes', 'Ctrl+V', 'roll'],
  ['roll.dup', 'Duplicate to the right', 'Ctrl+B', 'roll'], ['roll.quantDlg', 'Quantize…', 'Alt+Q', 'roll'], ['roll.quant', 'Quick quantize', 'Ctrl+Alt+Q', 'roll'], ['roll.quant2', 'Quick quantize (second key)', 'Ctrl+Q', 'roll'],
  ['roll.legato', 'Quick legato', 'Ctrl+L', 'roll'], ['roll.chop', 'Quick chop', 'Ctrl+U', 'roll'], ['roll.glue', 'Glue notes', 'Ctrl+G', 'roll'], ['roll.strum', 'Strum…', 'Alt+S', 'roll'], ['roll.slide', 'Draw slide notes on / off', 'S', 'roll'], ['roll.slideSel', 'Make the selected notes slide notes (or back)', 'Shift+S', 'roll'],
  ['roll.del', 'Delete notes', 'Delete', 'roll'], ['roll.up', 'Transpose up', 'ArrowUp', 'roll'], ['roll.down', 'Transpose down', 'ArrowDown', 'roll'],
  ['roll.octUp', 'Octave up', 'Ctrl+ArrowUp', 'roll'], ['roll.octDown', 'Octave down', 'Ctrl+ArrowDown', 'roll'], ['roll.sUp', 'Move selection up', 'Shift+ArrowUp', 'roll'], ['roll.sDown', 'Move selection down', 'Shift+ArrowDown', 'roll'],
  ['roll.left', 'Move selection earlier', 'Shift+ArrowLeft', 'roll'], ['roll.right', 'Move selection later', 'Shift+ArrowRight', 'roll'], ['roll.left2', 'Nudge earlier', 'ArrowLeft', 'roll'], ['roll.right2', 'Nudge later', 'ArrowRight', 'roll'],
  // Rack and Mixer
  ['rack.route', 'Route selected channels to free mixer buses', 'Ctrl+L', 'rack'], ['rack.clone', 'Clone selected channels', 'Alt+C', 'rack'], ['rack.group', 'Group selected channels', 'Alt+G', 'rack'], ['rack.del', 'Delete selected channels', 'Alt+Delete', 'rack'],
  ['mix.link', 'Link selected rack channels to this bus', 'Ctrl+L', 'mixer'], ['mix.rename', 'Rename this bus', 'F2', 'mixer'], ['mix.solo', 'Solo this bus', 'S', 'mixer'],
  ['mix.left', 'Move this bus left', 'Alt+ArrowLeft', 'mixer'], ['mix.right', 'Move this bus right', 'Alt+ArrowRight', 'mixer'], ['mix.render', 'Render armed buses to WAV', 'Alt+R', 'mixer'],
];
export const SCOPES = { global: 'Everywhere (single letters only when the typing keyboard is off)', song: 'Song timeline (click it first)', roll: 'Piano roll (click it first)', rack: 'Rack (click it first)', mixer: 'Mixer (click it first)' };
// Mouse gestures are fixed; listed in Settings so everything is in one place.
export const MOUSE = [
  ['Ctrl + wheel', 'Zoom the piano roll or song timeline at the cursor'], ['Alt + wheel', 'Piano roll row height'], ['Shift + wheel', 'Scroll sideways'],
  ['Ctrl + drag', 'Box-select notes or clips'], ['Ctrl + click', 'Add or remove one clip from the selection'], ['Shift + click', 'Add or remove one note'],
  ['Alt + drag', 'Move song clips without snapping'], ['Right-click', 'Erase a note or a step · delete a clip · knob menu (reset, type a value, automation, MIDI controller link)'], ['Double-click a note', 'Set its velocity'],
  ['Double-click a knob', 'Reset it (Shift-drag for fine control)'], ['Scroll on a bus box', 'Step a rack channel through the mixer buses'],
];

export function comboOf(e) {
  const k = e.code === 'Space' ? 'Space' : e.key.length === 1 ? e.key.toUpperCase() : e.key;
  if (['Control', 'Shift', 'Alt', 'Meta', 'Dead', 'Unidentified'].includes(k)) return '';
  return (e.ctrlKey || e.metaKey ? 'Ctrl+' : '') + (e.shiftKey ? 'Shift+' : '') + (e.altKey ? 'Alt+' : '') + k;
}
export const keyOf = id => prefs.keymap[id] ?? ACTIONS.find(a => a[0] === id)[2];
export function actionFor(e, scope) {
  const c = comboOf(e); if (!c) return null;
  const a = ACTIONS.find(x => x[3] === scope && keyOf(x[0]) === c);
  return a ? a[0] : null;
}
export function rebind(id, combo) {
  if (combo == null) delete prefs.keymap[id]; else prefs.keymap[id] = combo;
  savePrefs();
}
// Another action in the same place (or a global one) already using this key, if any.
export function clash(id, combo) {
  const me = ACTIONS.find(a => a[0] === id);
  const o = ACTIONS.find(a => a[0] !== id && keyOf(a[0]) === combo && (a[3] === me[3] || a[3] === 'global' || me[3] === 'global'));
  return o ? o[1] : '';
}
