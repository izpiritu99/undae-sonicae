// Shared UI for a plug-in hosted outside the app (CLAP natively, Windows VST through Wine): the chooser menu and the
// body of a plug-in slot. Used by mixer effect slots and by plug-in instrument channels in the rack.
import { h, btn, knob, menuAt, toast, bus } from '../util.js';
import { commit } from '../state.js';
import { engine } from '../audio/engine.js';
import { hostInfo, hostCall, plugServer } from '../audio/bridge.js';

// Changes made in a plug-in's own window arrive as 'plugdirty'; save the project shortly after the last one.
let saveT;
bus.on('plugdirty', () => { clearTimeout(saveT); saveT = setTimeout(() => commit(''), 400); });

// Scan both hosts and show a menu at `at`. want: 'fx' = effects only, 'inst' = instruments only. onPick gets the plug-in.
export async function choosePlugin(at, want, onPick) {
  const hi = await hostInfo();
  if (!hi) return toast(plugServer() ? `The plug-in server ${plugServer().addr} is not answering: check Settings › Plug-ins` : 'Plug-ins need the desktop app, or a computer sharing its plug-ins (Settings › Plug-ins)');
  const none = { plugins: [] }, [clap, vst, vst32] = await Promise.all([hi.url ? hostCall({ op: 'scan', paths: hi.bundled ? [hi.bundled] : [] }) : none, hi.win ? hostCall({ op: 'scan', paths: [hi.win.folder] }, 'vst2', 120000) : none, hi.win32 ? hostCall({ op: 'scan', paths: [hi.win32.folder] }, 'vst2', 120000, 32) : none]);
  const all = [...(clap.plugins || []), ...(vst.plugins || []), ...(vst32.plugins || [])].filter(pl => (want === 'inst' ? pl.instrument : want === 'fx' ? !pl.instrument : true));
  const what = want === 'inst' ? 'instrument plug-ins' : want === 'fx' ? 'effect plug-ins' : 'plug-ins';
  if (!all.length) return toast(hi.winPending ? 'Wine is still starting; try again in a few seconds' : `No ${what} found. Put .clap files in ~/.clap and Windows VST .dll files in ~/.vst-win`);
  const group = (title, test) => { const l = all.filter(test); return l.length ? [{ head: title }, ...l.map(pl => ({ label: pl.name + (pl.format === 'vst3' ? ' (VST3)' : '') + (pl.vendor ? ' · ' + pl.vendor : ''), fn: () => onPick(pl) }))] : []; };
  menuAt(at, [...(hi.remote ? [{ head: 'On ' + (hi.name || 'the plug-in server') + ' (windows open there)' }] : []), ...group('CLAP', pl => pl.format === 'clap'), ...group('VST3', pl => pl.format === 'vst3' && !pl.windows), ...group('Windows VST (through Wine)', pl => pl.windows && pl.bits !== 32), ...group('Windows VST, 32-bit (through Wine)', pl => pl.windows && pl.bits === 32)]);
}
export const slotFor = (pl, inst) => ({ path: pl.path, pid: pl.id, name: pl.name, fmt: pl.format, ...(pl.windows ? { windows: true } : {}), ...(pl.bits === 32 ? { bits: 32 } : {}), ...(inst ? { inst: true } : {}) });

// p: the stored slot ({ path, pid, name, fmt, state, v<id>... }); n: its live node (FX.plugin in audio/fx.js), if any.
// autoKey: automation target prefix ('fx:<bus>:<slot>:' or 'plug:<channel>:'), so knob moves can be recorded.
export function pluginBody(p, n, { want, onPick, onChange, autoKey }) {
  const info = n && n.info;
  const choose = e => choosePlugin(e.currentTarget, want, pl => { for (const k in p) delete p[k]; Object.assign(p, slotFor(pl, want === 'inst')); onPick(); });
  return [
    h('div', { class: 'hrow' }, btn(p.name || 'Choose plug-in…', choose, 'sm', 'Pick a CLAP or Windows VST plug-in installed on this computer'),
      info && info.gui && btn(n.guiOpen ? 'Close window' : 'Open window', async () => { const err = await n.window(!n.guiOpen); if (err) toast(err); }, 'sm' + (n.guiOpen ? ' primary' : ''), "Show the plug-in's own window"),
      n && n.status && h('small', null, n.status),
      want === 'fx' && n && n.bridge && h('small', { title: 'Audio through a plug-in is buffered, which delays this bus. The buffer grows by itself after a dropout.' }, `delay ${Math.round((n.bridge.latency + 128) / n.bridge.node.context.sampleRate * 1000)} ms`)),
    info && h('div', { class: 'krow wrap' }, info.params.filter(q => !q.hidden && !q.readonly).slice(0, 64).map(q => knob({ label: q.name.slice(0, 14), size: 'sm', min: q.min, max: q.max, step: q.stepped ? 1 : undefined, def: q.def, link: autoKey ? autoKey + 'v' + q.id : undefined, value: p['v' + q.id] ?? q.value, onInput: v => { p['v' + q.id] = v; n.set('v' + q.id, v); if (autoKey) engine.autoWrite(autoKey + 'v' + q.id, v); }, onChange }))),
    info && info.params.length > 64 && h('small', null, `Showing 64 of ${info.params.length} parameters; open the plug-in's window for the rest.`),
  ];
}
