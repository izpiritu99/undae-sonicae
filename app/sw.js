// Offline shell for the installed PWA. Network first so updates land immediately; cache is the fallback.
const CACHE = 'undae-sonicae-v7';
const SHELL = ['./', 'index.html', 'css/app.css', 'icons/icon-64.png', 'img/wordmark.png', 'img/wordmark-lg.png', 'manifest.webmanifest', 'js/main.js', 'js/util.js', 'js/state.js', 'js/presets.js', 'js/samples.js', 'js/midi.js', 'js/keys.js', 'js/ui/design.js',
  'js/audio/engine.js', 'js/audio/synth.js', 'js/audio/fx.js', 'js/audio/drums.js', 'js/audio/analysis.js', 'js/audio/stretch.js', 'js/audio/bridge.js', 'js/audio/encode.js', 'js/audio/mp3worker.js', 'js/vendor/lame.min.js', 'js/links.js', 'js/touch.js',
  'js/ui/rack.js', 'js/ui/pianoroll.js', 'js/ui/song.js', 'js/ui/mixer.js', 'js/ui/inst.js', 'js/ui/lab.js', 'js/ui/lib.js', 'js/ui/settings.js', 'js/ui/plugui.js'];
self.addEventListener('install', e => e.waitUntil(caches.open(CACHE).then(c => c.addAll(SHELL)).then(() => self.skipWaiting())));
self.addEventListener('activate', e => e.waitUntil(caches.keys().then(ks => Promise.all(ks.filter(k => k !== CACHE).map(k => caches.delete(k)))).then(() => self.clients.claim())));
self.addEventListener('fetch', e => {
  if (e.request.method !== 'GET') return;
  e.respondWith(fetch(e.request).then(r => { const copy = r.clone(); caches.open(CACHE).then(c => c.put(e.request, copy)); return r; }).catch(() => caches.match(e.request)));
});
