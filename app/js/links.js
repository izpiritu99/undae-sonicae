// Links from MIDI controller knobs and faders to anything that can be automated: bus and channel level and pan, effect
// and plug-in parameters, the master pitch. Stored in the project (S.p.links: [{ id, target, ch, cc }]). Several
// controls can share one MIDI knob, and one control can have several knobs.
import { S, commit } from './state.js';
import { engine } from './audio/engine.js';
import { toast, uid, bus } from './util.js';

let learning = null;   // target waiting for the next controller move

export function learn(target) {
  learning = target;
  toast('Move a knob or fader on your MIDI controller… (Esc cancels)', 8000);
}
export const cancelLearn = () => { if (learning) { learning = null; toast('Link cancelled'); } };
export const linksFor = target => (S.p.links || []).filter(l => l.target === target);
export function unlink(target) { S.p.links = (S.p.links || []).filter(l => l.target !== target); commit(''); toast('Controller link removed'); }

// Called for every MIDI control change. Returns true when it was used for linking or a link.
export function onControl(ch, cc, value) {
  if (learning) {
    const target = learning; learning = null;
    (S.p.links ||= []).push({ id: uid(), target, ch, cc }); commit('');
    const tg = engine.autoTarget(target);
    toast(`Linked CC ${cc} (channel ${ch + 1}) to ${tg ? tg.label : target}`);
    apply(target, value); return true;
  }
  let used = false;
  for (const l of S.p.links || []) if (l.cc === cc && l.ch === ch) { apply(l.target, value); used = true; }
  return used;
}
function apply(target, value) {
  const tg = engine.autoTarget(target); if (!tg) return;
  const v = tg.min + (tg.max - tg.min) * value / 127;
  tg.obj[tg.key] = v; engine.autoWrite(target, v);
  if (engine.graph) engine.graph.sync();
  for (const k of document.querySelectorAll(`[data-link="${CSS.escape(target)}"]`)) if (k.set) k.set(v);
  bus.emit('linked', { target, v });
}
