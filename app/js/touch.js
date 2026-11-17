// Touch screens (Android, tablets, touch laptops): everything a right-click or a key does must be reachable by finger.
// - Long-press anywhere = right-click: a `contextmenu` event at the finger, so every right-click menu works by touch.
//   The browser sometimes sends its own on a long-press; only one reaches the page per press.
// - <html data-touch="1"> while the last pointer was a finger, so CSS can show touch-only controls (edit buttons) and
//   bigger hit targets even on a touch laptop with a mouse attached.
// - pinch(el, onZoom, onPan): two-finger zoom / pan for scrolling views that keep one-finger native scrolling.
export const touch = { last: matchMedia('(pointer: coarse)').matches ? 'touch' : 'mouse' };
const root = document.documentElement;
const mark = t => { if (touch.last !== t) { touch.last = t; root.dataset.touch = t === 'touch' ? '1' : '0'; } };
root.dataset.touch = touch.last === 'touch' ? '1' : '0';

const HOLD = 520, SLOP = 10;
let press = null;   // { id, x, y, timer, fired, target }
addEventListener('pointerdown', e => {
  mark(e.pointerType === 'touch' || e.pointerType === 'pen' ? 'touch' : 'mouse');
  if (e.pointerType !== 'touch') return;
  if (press) { clearTimeout(press.timer); press = null; return; }   // a second finger: a gesture, not a long-press
  const p = press = { id: e.pointerId, x: e.clientX, y: e.clientY, target: e.target, fired: false, t0: performance.now() };
  // When the timer fires, wait one frame first: if the page was busy (a first note loading its sound, say) the finger may
  // already be up with its event still queued, and that tap must stay a tap.
  p.timer = setTimeout(() => requestAnimationFrame(() => setTimeout(() => {
    if (press !== p || performance.now() - p.t0 < HOLD) return;
    p.fired = true;
    const t = document.elementFromPoint(p.x, p.y) || p.target;
    if (navigator.vibrate) try { navigator.vibrate(12); } catch { /* not allowed */ }
    t.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, cancelable: true, clientX: p.x, clientY: p.y, button: 2, buttons: 0 }));
  })), HOLD);
}, true);
addEventListener('pointermove', e => { if (press && e.pointerId === press.id && Math.hypot(e.clientX - press.x, e.clientY - press.y) > SLOP) { clearTimeout(press.timer); if (!press.fired) press = null; } }, true);
const end = e => {
  if (!press || e.pointerId !== press.id) return;
  clearTimeout(press.timer);
  const p = press; press = null;
  if (p.fired) {   // the menu is open: the finger lifting must not also click whatever is under it
    const stop = ev => { ev.stopPropagation(); ev.preventDefault(); };
    addEventListener('click', stop, { capture: true, once: true }); setTimeout(() => removeEventListener('click', stop, { capture: true }), 400);
  }
};
addEventListener('pointerup', end, true); addEventListener('pointercancel', end, true);
// The browser's own long-press menu: let it through when ours has not fired yet (and cancel ours), drop it afterwards.
addEventListener('contextmenu', e => {
  if (!e.isTrusted || touch.last !== 'touch') return;
  if (press && (press.fired || performance.now() - press.t0 < 400)) { e.stopPropagation(); e.preventDefault(); return; }   // ours already, or too early to be a long-press
  if (press) { clearTimeout(press.timer); press.fired = true; }
}, true);

// Two-finger pinch (zoom by the horizontal spread) and drag (pan) on an element that scrolls natively with one finger.
export function pinch(el, onZoom, onPan) {
  let g = null;
  const pts = t => [...t].slice(0, 2).map(x => [x.clientX, x.clientY]);
  el.addEventListener('touchstart', e => { if (e.touches.length === 2) { const [a, b] = pts(e.touches); g = { d: Math.max(30, Math.abs(a[0] - b[0])), cx: (a[0] + b[0]) / 2, cy: (a[1] + b[1]) / 2 }; } }, { passive: true });
  el.addEventListener('touchmove', e => {
    if (!g || e.touches.length !== 2) return;
    e.preventDefault();
    const [a, b] = pts(e.touches), d = Math.max(30, Math.abs(a[0] - b[0])), cx = (a[0] + b[0]) / 2, cy = (a[1] + b[1]) / 2;
    if (Math.abs(d / g.d - 1) > 0.02) { onZoom(d / g.d, cx); g.d = d; }
    if (onPan) onPan(cx - g.cx, cy - g.cy);
    g.cx = cx; g.cy = cy;
  }, { passive: false });
  el.addEventListener('touchend', e => { if (e.touches.length < 2) g = null; });
}
