# Undae Sonicae

A pattern-based DAW for Linux and Android built on one Web Audio codebase (`app/`), wrapped by Electron for
Linux and Capacitor for Android. No build step: `app/` is plain ES modules.

## Run it

| Target | Command | Needs |
| --- | --- | --- |
| Browser (any OS) | `npm start` then open http://localhost:5173 | Node 18+ |
| Linux desktop (dev) | `npm install` then `npm run desktop` | Node 18+ |
| Linux AppImage + .deb | `npm run dist:linux` (run on Linux or WSL) → `dist/` | Node 18+ |
| Android APK | `npm install`, `npm run android:init` (once), `npm run android:apk` → `android/app/build/outputs/apk/debug/` | Node 18+, JDK 17+, Android SDK |
| Android without building | Host `app/` over HTTPS, open in Chrome, "Add to Home screen" (installable PWA, works offline) | – |

After changing files in `app/`, run `npm run android:sync` before rebuilding the APK.

The Electron and Capacitor wrappers were written but **not built or run on the authoring machine** (no Node or
Android SDK there). The app itself was tested in Chromium.

## What's in it

- **Rack** – channel rack / step sequencer: per-channel level, pan, mute, solo, colour; global swing; per-step
  velocity (right-click a step); fill, Euclidean, shift and randomise tools; 1–64 bar patterns.
  Each channel has a bus box (M, 1..n, A/B) that routes it to a mixer bus; swing works on a 1/16 or 1/8 grid.
- **Piano Roll** – draw / select / slice / erase / pan tools, velocity lane, snap (incl. triplets), scale
  highlight + scale lock, ghost notes, quantize (hard, soft, lengths), legato, chop, arpeggiate, strum, humanize,
  reverse, copy/paste/duplicate, key detection from notes.
- **Song** – arrange pattern clips on tracks; PAT / SONG switch in the transport.
- **Mixer** – master, insert and two send buses: level, pan, mute, solo, polarity invert, mono, stereo width,
  sends, peak meters, and an FX chain per bus where each slot can be bypassed, reordered or removed
  (EQ, filter, compressor, limiter, reverb, delay, chorus, phaser, distortion, bitcrush).
- **Instrument** – *Synth*: 3 oscillators, unison, sub, noise, FM, multimode filter with drive, three
  envelopes, two LFOs, a 6-slot modulation matrix, mono/poly, glide, presets. *Sampler*: any uploaded audio
  becomes a playable instrument (root note, tune, start, attack/release, hold, reverse, choke groups,
  automatic root-pitch detection).
- **Audio Lab** – record the mic or load a file, then: detect key (with Camelot code) and tempo; convert
  audio to MIDI (melody mode, plus an experimental chord mode); or play any instrument live with your voice.
- **Library** – synthesised 808-style kit (16 sounds), stock keys and sub, your uploads.
- **MIDI** – USB/Bluetooth keyboards via Web MIDI (notes, pitch bend, mod wheel, sustain), live record into
  the pattern with optional quantize, `.mid` import/export. On-screen and typing keyboards.
- **Project** – undo/redo, autosave, project files with embedded samples, WAV export (offline render).
- **Settings** – 8 themes, custom accent, corner radius, UI scale, density, step shape, note colouring,
  grid contrast, deck layout, latency / look-ahead / polyphony / meter rate.

## Added since the first version

- Free mixer routing: sends from any bus to any bus, optional master link, add insert or send buses, bus presets, dB peak readouts.
- Peak controller (ducking), whole-signal or one frequency band, triggered by another bus.
- Automation lanes for bus and channel level / pan: draw them in the Song view or record fader moves.
- Record dialog (notes, microphone audio, automation, loop record); time shown as B:S:T or M:S:CS.
- Rack row views (steps, notes, waveform), pattern length in beats, split a pattern by channel.
- Sample folders in the Library, drag and drop onto the Rack and Song tracks, Sound Design view.
- Rebindable hotkeys (Settings > Hotkeys).

## Known limits

- **No VST hosting.** VSTs are native desktop plug-ins; they cannot load in a web runtime and do not exist on
  Android at all. The mixer's FX slots use the built-in effects instead.
- Audio-to-MIDI is reliable for one-note-at-a-time sources (voice, bass, lead). Chord mode is a best-effort
  spectral transcription and will miss or mis-octave notes on dense material.
- Synth edits apply to the next note played (filter cutoff/resonance also update held notes).
- No tempo or time-signature automation: one tempo and one signature per project. Recorded takes follow tempo changes with a pitch-preserving stretch (best within about half to double speed).
- **No ASIO.** ASIO / ASIO4ALL are Windows-only drivers. The app uses the OS audio system (PipeWire, PulseAudio, ALSA or JACK on Linux; AAudio on Android); pick the output device, sample rate and buffer in Settings > Audio.
- Audio recording needs SONG mode: arm a mixer bus (IN, then the red dot), turn on Rec and press play.
- Android USB-MIDI through the Capacitor WebView depends on the device's WebView version; the Chrome-installed
  PWA is the safer route if a keyboard is not detected.

## Layout

```
app/            the whole application (index.html, css/, js/audio, js/ui)
desktop/main.js Electron shell
tools/          static server, Android manifest patch
```
