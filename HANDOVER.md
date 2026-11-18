# Undae Sonicae – handover for continuing on Ubuntu

Written on 2026-11-12 at the end of the Windows session. Read this first.

**Continuing on Windows (from 2026-11-17)?** The task there is a native Windows build of the C++ / JUCE edition. Start
with the last section, "Native edition on Windows", then read "Native edition (C++ / JUCE)" for the background.

## What this is

A pattern-based DAW. One web codebase (plain ES modules, no build step) in `app/`, wrapped by Electron for Linux
(`desktop/main.js`). The owner's focus was Linux first; since 2026-11-16 the Android wrapper (`android/`, Capacitor) is being
built again with a touch-friendly UI (section 4e).

## First steps on Ubuntu

The USB drive is NTFS. Copy this whole folder into your home directory first (for example `cp -r /media/$USER/'Undae Sonicae'/undae-sonicae ~/`) and work there: `npm install` needs symbolic links and Linux file permissions, which an NTFS drive does not handle well.

```bash
sudo apt install nodejs npm        # Node 20 or newer; use nodesource / nvm if apt's is older
npm install                        # node_modules was NOT copied (it was installed on Windows)
npm run desktop                    # run the real app in Electron
npm start                          # or: serve app/ at http://localhost:5173 for a browser
npm run dist:linux                 # build AppImage + deb into dist/
```

`.claude/launch.json` now runs `node tools/serve.js` on port 5173 (on Windows it used Java's `jwebserver`).

## State of the work

Everything below was tested in Chromium on Windows, mostly with scripted checks. On Linux only the checks listed under
"Linux results" have been run.

Built and browser-tested:
- Rack (step / notes / waveform row views, pattern length in beats, swing on a 1/16 or 1/8 grid, per-channel bus box)
- Piano roll (tools, snap, scale lock, arpeggiate, quantize; "Follow" playback scrolling is off by default)
- Song view (pattern clips, audio clips, snap, Ctrl+wheel zoom, Ctrl+drag selection, automation lanes, drag-and-drop audio)
- Mixer (free routing: sends from any bus to any bus, master link toggle, insert/send buses, presets, peak dB readouts,
  per-bus microphone input with arm and monitor)
- Effects incl. "Peak controller (duck)" driven by an AudioWorklet envelope follower
- Automation of bus/channel level and pan and of any numeric effect parameter (drawn or recorded)
- Synth (12 wave shapes, morph, two filters, shaper, 3 LFOs, 2 mod envelopes, macros, 8-slot matrix)
- Sound Design view, Audio Lab (key/tempo detection, audio to MIDI), Library with sample folders
- Time signatures, pitch-preserving time-stretch of recorded takes, rebindable hotkeys, 1-4 decks plus a floating deck
- Settings tabs: Theme (incl. background photo), General, Audio (output device, quality), MIDI, Hotkeys, File, Plug-ins, Project
- Starts with a blank project; previous session is under the project menu > Reopen last session

Never tested on real hardware: microphone recording, output-device switching, MIDI keyboards, sample-folder access.

## Linux results (2026-11-12, Ubuntu 26.04.1, Wayland session, PipeWire, Node 24, Electron 33.4.11)

- `npm install` and `npm run dist:linux` worked unchanged. `dist/` holds `Undae Sonicae-0.1.0.AppImage` and
  `undae-sonicae_0.1.0_amd64.deb` (the deb has not been installed).
- `npm run desktop`, `dist/linux-unpacked/undae-sonicae` and the AppImage all start with the Chromium sandbox on;
  `--no-sandbox` was not needed.
- **The AppImage needs FUSE 2**, which Ubuntu 26.04 does not install by default: `sudo apt install libfuse2t64`.
  Without it, it runs with `--appimage-extract-and-run`.
- Scripted checks in the packaged build (DevTools protocol, `--remote-debugging-port`), no console errors:
  - the UI renders; the demo project plays through the app's engine (master peak about 0.35, 48 kHz, output latency
    about 44 ms on built-in audio through PipeWire's Pulse layer);
  - `engine.setSink()` to the built-in output and back to default succeeds (only one output device was present);
  - `getUserMedia` opens the microphone with processing off, and device names are listed;
  - Web MIDI is granted and lists ALSA ports (only "Midi Through" was present).
- Still not done: an actual microphone take (the system capture source was muted), a real MIDI keyboard, a second
  output device or JACK, sample-folder access, and a by-ear listening check.

## Open items, in the owner's priority order

1. ~~AppImage~~ Done, see above. The Windows-built `.tar.gz` in `builds/` is obsolete.
2. **Hardware tests on Linux** left over from the list above: mic take per bus, MIDI keyboard, second output / JACK.
3. **Native Linux plug-ins.** CLAP effects and instruments work, with windows, automation and export; LV2 and Linux
   VST3 do not yet.
   - `native/host.c` is the host: a separate process with a loopback WebSocket (`native/PROTOCOL.md`), CLAP only.
     `native/build.sh` builds it and `native/testplug.c` (a gain effect and a sine synth) into `native/bin/`;
     `npm run test:host` runs 21 end-to-end checks. CLAP headers are vendored in `native/include/clap` (MIT).
   - Electron main (`desktop/main.js`, `tools/host.js`) starts the host and the app's server answers `/host.json` with
     its port and token. Packaged builds carry `undae-host` and `undae-test.clap` in `resources/`.
   - `app/js/audio/bridge.js` is the audio path (AudioWorklet, shared ring buffers, Worker). It needs
     `SharedArrayBuffer`, so `tools/serve.js` sends COOP/COEP headers.
   - Mixer: effect type "Plug-in (CLAP)" (`FX.plugin` in `app/js/audio/fx.js`, UI in `pluginBody` in
     `app/js/ui/mixer.js`). The slot stores `{ path, pid, name }` plus `v<paramId>` for each changed parameter.
   - Tested in Electron, dev and packaged: scan, load, audio through the master bus, parameter changes. The bridge
     buffer is 1024 frames (about 21 ms on that bus); 512 gave a few dropouts per 20 s under a full project.
   - Only the bundled test plug-ins have been run. No third-party CLAP plug-in has been tried.
   - Plug-in windows work (X11, embedded or floating): "Open window" in the slot. The host gives plug-ins timers and
     file-descriptor callbacks, sends changes made in the window back to the app (`param` events), and the app stores
     the plug-in's opaque state in the slot (`state`) when the plug-in marks it dirty. X11 is declared by hand in
     `native/xmini.h` and linked as `-l:libX11.so.6`, so `libx11-dev` is not needed. `native/xpoke.c` is a test helper
     that clicks in or closes a window; `npm run test:host` covers windows too (30 checks). The session is Wayland, so
     plug-in windows go through XWayland; resizable windows and floating windows are untested (the test plug-in has
     a fixed-size embedded one).
   - Plug-in instruments are rack channels: channel `type: 'plugin'` with the slot in `c.plug` (same shape as an effect
     slot plus `inst: true`). The graph gives the strip an `FX.plugin` node (`s.plug`) that survives graph rebuilds;
     `Graph.trigger` sends `note` messages stamped with their audio frame (`bridge.frameAt`), which also cancels the
     bridge delay for sequenced notes (the test synth's onsets land within the 5 ms measuring error). Live keys are
     late by the bridge delay. Shared UI is `app/js/ui/plugui.js` (chooser filtered to effects or instruments, slot
     body); the Rack's "+ Channel" menu and the Instrument view use it. Works for CLAP and for VST2 through Wine
     (FB-3300 plays a pattern).
   - Dropouts: measured cause is the audio thread itself being held up 20 to 40 ms (seen at play start and now and
     then under load on this 2-core machine), after which it catches up in a burst and drains the bridge buffer
     before replies return. The bridge now (a) waits up to 3 ms in the worklet for the reply when the buffer is empty,
     and (b) grows the buffer by 512 frames after each real dropout, up to 4096; here it settles at 1536 to 2048
     frames with roughly one dropout per minute or fewer. `bridge.stats()` gives `underruns`, `waits`, `latency`,
     `audioGapMs`; the effect slot shows the current delay. Test runs were made with the GPU process failing to
     start (software rendering), which adds CPU load; a normal launch may behave better.
   - Export includes plug-ins. `engine.render` first renders each plug-in instrument through its host from its notes
     (`renderOffline` in `bridge.js`: a plain WebSocket, 1024-frame blocks, faster than real time, notes stamped
     with their frame), then plug-in effects layer by layer: one extra offline pass per layer records what reaches
     those slots on extra output channels (`renderPass` with `taps`), the audio goes through the host, and the slot
     then plays the result (`offlineAudio` map in `fx.js`). Checked exactly with the gain test plug-in: 0.5 on the
     master gives 0.5 x the dry export with zero error, bus then master 0.25, three in one chain, a bypassed slot,
     and a missing plug-in (export continues and the toast says what was left out). Instrument notes start on the
     exact sample. FB-3300 plus TAL export 4.5 s in about 0.7 s.
   - Once, in a packaged test run, FB-3300 returned a sample of about 1e38 during an export while a second FB-3300
     instance was being opened for live playback; it did not recur in 18 further exports. Two guards went in: the
     Windows host takes a process-wide lock (`load_lock`, an SRW lock) exclusively while a plug-in is opened, started
     or closed and shared while any instance processes, and both hosts replace any sample outside -64..64 with
     silence. The cause is a guess (plug-in instances sharing tables); the lock means opening a plug-in briefly
     pauses the other Windows plug-ins.
   - Controllers: protocol op `midi` (`data: [status, d1, d2]`, optional `at`). `engine.setBend/setWheel/setSustain`
     send pitch bend, CC 1 and CC 64 to the selected channel when it is a plug-in instrument (the app still holds
     note-offs itself while the pedal is down). CLAP: sent as `CLAP_EVENT_MIDI` when the plug-in accepts MIDI;
     otherwise pitch bend becomes a tuning note expression (2 semitones) and controllers are dropped. VST2: MIDI
     events. VST3: through the controller's `IMidiMapping`. The test synth takes MIDI and halves its level at full
     mod wheel, so tests can see it. FB-3300's default patch has `PB Range` 0, so bend does nothing until raised.
   - Automation of plug-in parameters: targets `fx:<bus>:<slot>:v<id>` (effects) and `plug:<channel>:v<id>`
     (instruments). `engine.autoTarget` reads name and range from the loaded plug-in, or from `slot.meta` (written by
     `engine.rememberParam` when a lane is made). Song view "+ Automation" > "Plug-in parameters" opens a per-plug-in
     menu. Knob moves and changes in the plug-in's window are recorded while recording automation. Export sends the
     lane as timed `param` messages every 512 frames (CLAP: exact; VST2 host splits processing at the change; VST3:
     parameter queues). Live playback applies values without timing, so instrument automation is late by the bridge
     delay. Checked exactly against an expected ramp (worst error 5e-4, within one 1024-frame block step).
   - Not done: delay compensation for effect buses, restarting the host if a plug-in crashes it (all instances
     share one process), LV2 and Linux VST3. `native/vst3c.h` (the VST3 interfaces in C, both byte orders) is ready
     for a Linux VST3 host, but there was no Linux VST3 plug-in to test with.
4. **Windows VSTs through Wine.** VST2 and VST3, 64-bit and 32-bit, effects and instruments, with windows.
   - `native/win/vsthost.c` is a Windows program that runs under Wine, loads VST2 `.dll` files and speaks the same
     protocol as `host.c` (format `vst2`), so the app's bridge connects to it directly. The VST2 interface is declared
     by hand from its public layout; no Steinberg SDK.
   - It is built with the **Linux** gcc and ld, no mingw: `-mabi=ms -ffreestanding`, ELF-only sections stripped with
     objcopy, then `ld -m i386pep` linked straight against Wine's own `msvcrt.dll`, `kernel32.dll`, `ws2_32.dll`,
     `advapi32.dll`. All Windows functions are declared by hand in `native/win/winshim.h` (`long` is 8 bytes there;
     keep stack frames small, there are no stack probes). `native/build.sh` skips it when Wine is absent.
   - `tools/host.js` starts it with `wine` in the app's own prefix `~/.local/share/undae-sonicae/wine` (not `~/.wine`);
     it exits when its stdin closes. `/host.json` now carries `win: { port, token, folder }` and `winPending`.
   - Plug-ins are looked for in `~/.vst-win`. The owner's two test plug-ins were copied there: `TAL-Bitcrusher.dll`
     and `fb330064.dll` (FB-3300 synth). `npm run test:vsthost -- ~/.vst-win` loads each plug-in found and checks
     audio, a parameter, state and timing; both pass. In the app, TAL BitCrusher was run on the master bus.
   - The bridge buffer for `vst2` is 1536 frames (about 32 ms): at 1024, Wine gave 0 to 2 dropouts per 20 s. Socket
     calls are costly under Wine, so the host reads in bulk and sends each message with one call.
   - Plug-in windows work: `gui` opens a Win32 window on the host's main thread (`effEditOpen`, a 30 ms timer for
     `effEditIdle`, message pump in the main loop), which Wine shows as a normal desktop window. Closing it sends
     `gui` and `dirty` events, and the app then stores the plug-in's chunk in the slot. Both test plug-ins' windows
     open, render and close (checked with `xwininfo` and a pixel capture via `xpoke shot`). Mouse input inside a
     Wine window was not scripted; it has not been tried by hand either.
   - 32-bit: the same source built with `-m32 -fleading-underscore -malign-double -mincoming-stack-boundary=2
     -fno-merge-constants` and `ld -m i386pe` against Wine's `i386-windows` DLLs, giving `undae-vsthost32.exe`
     (Wine runs it in its WoW64 mode). Windows API calls are stdcall (`WINAPI` in `winshim.h`). There is no 32-bit
     libgcc, so no 64-bit division or double-to-64-bit conversions in that code. Merged string sections came out
     wrong in the PE link (hence `-fno-merge-constants`); Windows only promises 4-byte stack alignment (hence
     `-mincoming-stack-boundary=2`). Each host lists only DLLs of its own bitness (`pe_bits`); slots store
     `bits: 32` and the app routes them to `win32` in `/host.json`. `npm run test:vsthost -- --32 ~/.vst-win`.
   - VST3 (`native/win/vst3win.h`, interfaces in `native/vst3c.h` from Steinberg's MIT-licensed headers): component
     and controller (connected, component state copied in), `IMidiMapping` for controllers, memory streams, host
     messages and attribute lists, parameter queues and event lists, a process context at 120 bpm, `IPlugView` in
     a Win32 window with `IPlugFrame` resizing. State is `"V3"` + component state + controller state. Bundle folders
     (`Contents/x86_64-win/`) and single-file `.vst3` both scan. `fb3300.vst3` passes the host tests and, in the app,
     plays from the Rack, opens its window and exports; its volume parameter, pitch bend and state restore were
     checked by measurement.
   - `~/.vst-win` now also holds `fb3300.dll` (32-bit) and `fb3300.vst3`.
   - Not done: scan caching (every "Choose plug-in" loads each `.dll`), and a crash in one plug-in takes the whole Windows
     host down.
4b. **Round of 2026-11-13 (owner's list).**
   - Stop: the app sends `panic` to plug-in instruments. The CLAP host calls `reset`. The Windows host replaces the
     instance (state saved, loaded again, state restored, window reopened, queued notes dropped), because FB-3300
     keeps its release tail through `effMainsChanged`, `setActive` cycles and MIDI CC 120/123. Costs ~200 ms.
   - Underruns: Settings > Audio > CPU > "Multi-threaded processing" (stored by the main process in
     `<userData>/engine.json`, served at `/engine.json`, applies after restart). On: Chromium
     `AudioWorkletRealtimeThread`, no background throttling, plug-ins process in parallel. Off: hosts get
     `UNDAE_SERIAL=1` and process one plug-in at a time. Measured: the realtime flag did not visibly change audio-thread
     stalls on this machine (20-40 ms, about one dropout per 45 s). Plug-in bridges now remember the buffer size they
     grew to (`localStorage lw.pluglat`) and start there next time.
   - Mixer "+": left click adds an insert bus, right click a send bus.
   - Templates: `lw.templates` (`saveTemplate`, `fromTemplate`, `deleteTemplate` in state.js); project menu "Save as
     template…" / "New from template"; Settings > Project lists them and offers them as the start-up choice
     (`prefs.startup = 'tpl:<name>'`). Samples are referenced, not copied.
   - Deck tabs: decks are size containers; under 900px the tabs show icons only (active one keeps its label), the
     wheel scrolls the row sideways, and a thin scrollbar shows.
   - Rack: right-click (or right-drag) removes steps; the old step-velocity menu is gone. Piano Roll: double-click a
     note for a velocity menu (applies to the selection when the note is selected); right-click already deleted.
   - On-screen keyboard: drag its top edge to resize; ⇱ makes it a floating panel (drag the left strip to move, the
     corner to resize); octaves follow the size. Kept in `prefs.keysUi`.
   - Sound Design: an Effects card (any built-in mixer effect except the peak controller, and CLAP/Windows VST
     effects with up to 32 knobs), applied in order, with a Ring-out length; the result is stereo.
   - Start-up screen shows `app/img/wordmark-lg.png` (900 px tall cut of the wordmark).
   - Linux VST3 (`native/vst3lin.h`, included by host.c; shared host objects in `native/vst3host.h`): bundles
     `Contents/x86_64-linux/<name>.so` with `ModuleEntry`/`ModuleExit`, non-COM interface IDs, window embedded in an
     X11 window the host owns ("X11EmbedWindowID"), `IRunLoop` offered through the frame (fds and timers run by
     host.c's `wait_events`, entries carry `v3h`). Notes, MIDI and parameter values use host.c's CLAP-shaped queue and
     are converted per block. Scanned in `~/.vst3`, `/usr/lib/vst3`, `/usr/local/lib/vst3`. Slots carry
     `windows: true` for Wine plug-ins (`isWindowsSlot` in bridge.js; older slots: path under `/.vst-win/`).
   - Tested with Surge XT 1.3.4 built from `~/Downloads/surge-src-1.3.4/surge` (cmake + ninja; GCC 15 needed
     `-Wno-error=discarded-qualifiers` etc. because bundled LuaJIT/sqlite use `-Werror`), installed to `~/.vst3`, factory
     data copied to `~/.local/share/surge-xt`. Loads (2855 parameters), plays, volume parameter, pitch bend (2 semitones,
     measured), sustain pedal, state save/restore, window (1826x1138, captured), Surge XT Effects passes audio; in the
     app it plays from the Rack, opens its window and exports.
   - Stop: soft reset did not silence Surge either, so the Linux host also replaces VST3 instances on `panic`
     (same capture-and-reload as the Windows host). CLAP plug-ins still get `reset`.
4c. **Round of 2026-11-15 (owner's list and screenshots of another DAW's menus).**
   - Note ids: notes imported from MIDI had no `id`, so the Piano Roll's id-based selection matched all of them (the
     "velocity changes every note" bug). `noteIds()` in state.js now runs in `migrate` and on every `commit`.
   - Menus (util.js) take `{ label, sub: [...] }` for sub-menus. Every knob has a right-click menu (reset, type a
     value, copy/paste); knobs and faders given a `link` target (an automation target string) also get "Create
     automation lane", "Start the song at this value" and "Link to MIDI controller" (`knobHooks.items`, set in main.js).
   - MIDI controller links: `app/js/links.js`, stored in `S.p.links` `{ target, ch, cc }`; midi.js passes every CC to
     `onControl` first. Any number of links per knob and per target. Targets: `bus:`, `ch:`, `fx:` (incl. plug-in
     `v<id>`), `plug:`, and `master:pitch`. Linked knobs update through `[data-link]` elements' `.set()`.
   - Top bar: master pitch knob (`S.p.masterPitch`, cents on built-in voices, whole semitones on plug-in notes), CPU
     and RAM of the app plus plug-in hosts (`/stats.json`, `loadStats` in tools/host.js: Electron app metrics +
     /proc for hosts, CPU as % of all cores) with a 60 s history, and the number of playing voices.
   - Settings > Audio: input device (`prefs.inputId`), used by buses set to "Default input".
   - Song: snaps are note values (1/4 … 1/64, triplets) as fractions of a bar; tools draw / paint / delete / mute / slip
     / slice / select / zoom / play (letters P B D T S C E Z Y); clip and pattern colours (`clip.color`, `pattern.color`);
     clip groups (`clip.group`); pattern-clip slip offset (`clip.off`, bars, honoured by `collect`); muted pattern clips
     are now silent (they were not before); ruler drag = time selection (`S.songSel`), click = play marker; time markers
     (`song.markers`); Insert / Delete space; copy / paste; quick quantize; make unique; arrangements (`P.arrs`,
     `P.arrCur`, the current one lives in `P.song`); the "☰" Song menu has Edit, Tools, Snap, Select, Group, Zoom, Time
     markers, Arrangements.
   - Piano Roll: tools paint / mute (`note.mute`, skipped by `collect`) / zoom / play; chord and scale stamps; loop mode
     (`pattern.loop {on,a,b}` in ticks; ruler drag sets it; `collect` and `loopLen` honour it); control lane switch
     (velocity, note pan `note.pan`, note fine pitch `note.fine` in cents, both applied per voice in `Graph.trigger`);
     tools glue, flam, limit, flip, randomize, scale levels, LFO, chord progression, quantize and strum dialogs.
   - Mixer: a scrolling waveform on each strip (min/max of each meter frame, newest on the right, scaled to the loudest column shown), Mixer hotkeys (Ctrl+L link selected rack channels,
     F2, S, Alt+arrows, Alt+R renders each armed bus to its own WAV by muting what does not reach it).
   - Rack: Ctrl+click names for several channels (`S.chSel`, `selectedChannels()`); Ctrl+L gives each its own bus,
     Alt+C clones, Alt+G sets `channel.group`, Alt+Del deletes. The rack toolbar's Group filter (shown once a group exists)
     shows All / Unsorted / one group; the choice is `P.rackGroup` ('' all, '-' unsorted). The row menu has Group… too.
   - Hotkeys (keys.js) follow the owner's list. Single-letter global keys (L, R) are ignored while the typing keyboard is
     on, because those letters play notes. Ctrl+Z toggles undo/redo, Ctrl+Alt+Z steps back. Ctrl+Space play/pause,
     Ctrl+P count-in (one bar of clicks), Ctrl+I waits for the first note, Ctrl+H panic, F1 shortcut list, F8 plug-in
     picker, F10/F11 open Settings tabs (`settab` event), F12 closes floating panels.
   - Compressed export (`app/js/audio/encode.js`): MP3 through LAME (lamejs 1.2.1, LGPL, vendored unchanged as
     `app/js/vendor/lame.min.js` + `LAME-LICENSE.txt`, run in `audio/mp3worker.js`; rates other than 32/44.1/48 kHz are
     resampled first) and Ogg Opus via WebCodecs `AudioEncoder` with a small Ogg writer. Ctrl+Shift+R uses
     `prefs.compFormat` (default mp3); bitrates `prefs.mp3Kbps` (320) / `prefs.opusKbps` (192) in Settings › Audio ›
     Quality. The File menu offers both. A 5 s demo pattern at 192 kbps: 120 KB, encoded in 0.8 s, decodes to 5.14 s.

### 4d. Round of 2026-11-16 (the items left over from 4c)
   - Piano Roll lanes: release (`note.rel`), filter cutoff (`note.cut`), resonance (`note.res`), all -1..1 around the
     channel's own value (0 = unchanged, stored only when non-zero). Synth: release x4^rel on the amp and mod envelopes,
     cutoff +3600*cut cents on both filters, Q x4^res. Sampler: release x4^rel, and a per-note low-pass (20 kHz down to
     ~160 Hz for cut -1, Q 0.707*4^res) only when cut < 0 or res is set. Plug-ins: CC74 / CC71 before the note (and back
     to 64 once a later note has none), live and in offline renders (`renderOffline` now accepts `{frame, data}` MIDI).
     The LFO dialog can drive these lanes too. `noteOpts(e)` in engine.js is what `collect` events hand to `trigger`.
   - Slide notes (`note.slide`): the Slide button / S key makes new notes slide notes, Shift+S toggles the selection
     (also in Tools). In `Graph.trigger` a slide note starts no voice when the channel has voices sounding: each one's
     `v.slide(note, t, dur)` glides its pitch offset to the slide note's pitch over the slide note's length
     (`cancelAndHoldAtTime` + linear ramp). With nothing sounding it plays as a normal note. Drawn dashed with a triangle.
     Plug-in instruments play slide notes as ordinary notes (no pitch-bend range is known for them).
   - Song: View in the ☰ menu (track height presets, Taller / Shorter, Alt+wheel; `prefs.songTH`, CSS `--th`) and toggles
     for the picker panel, clip contents, automation lanes, time markers (`prefs.songPicker/songPreview/songAutos/
     songMarkers`, false = hidden). Clip contents hide by themselves below 30 px.
   - Picker panel: header with a ▾ menu (show patterns / audio clips, hide unused, sort by order / name / colour /
     length, hide the panel; `prefs.picker`). Audio clips used in the song are listed (one per sample); clicking one sets
     `S.songSrc = { audio }` so draw and paint place copies of it; patterns and audio both drag onto tracks
     (`text/x-undae-pick` = `pat:<id>` or `audio:<sample>`).
### 4e. Round of 2026-11-16 (b): rack playhead, touch, Android
   - Rack Notes / Wave views follow playback: one `.rplay` line over the rows (positioned from the first `.rview`
     canvas), and rows redraw each step so sounding notes light up in the Notes view (`drawView(r, playTick)`).
     `refresh()` walks `.row` elements only, because the playhead lives inside `rows`.
   - Touch (`app/js/touch.js`, loaded by main.js): long-press = `contextmenu` at the finger (520 ms; the timer waits a
     frame so a queued finger-up wins; the browser's own long-press menu is let through once, or dropped when ours fired
     or when it comes before 400 ms); `<html data-touch>` follows the last pointer type; `.touchonly` controls show only
     then (Piano Roll: All / Copy / Paste / Dup / Del; Song: Copy / Paste / Dup / Del); `pinch(el, onZoom, onPan)` for
     natively scrolling views (Song). Piano Roll: two fingers pinch (sideways = time zoom, up/down = row height) and pan;
     a long-press on a note opens a note menu (delete, duplicate, copy, velocity, mute, slide), on empty grid paste /
     select / tools; a long-press on a pattern clip opens its menu instead of deleting it. `cv.view` exposes the roll's
     scroll/zoom state for tests.
   - CSS for `(pointer: coarse)`: 40 px controls (36 / 46 with Compact / Roomy), 44 px menu rows, bigger knobs, toolbars
     scroll sideways instead of wrapping, tabs scroll on phones; phones sideways (coarse and height <= 560 px) get one deck
     and a one-row swipeable top bar (`narrow` media query in main.js).
   - Android (Capacitor 8, `android/`): no longer paused. Build: `npx cap sync android`, then in `android/`
     `JAVA_HOME=/opt/android-studio/jbr ./gradlew assembleDebug` (needs `android/local.properties` with `sdk.dir`).
     The wrapper is Gradle 9.1.0 because Android Studio's bundled JDK is 25 (Gradle 8.14 cannot run on it). Output is
     copied to `builds/undae-sonicae-0.2.0-android-debug.apk`. Checked on the `Medium_Phone_API_37.0` emulator
     (x86_64, Android 17): starts, edge-to-edge insets respected, tap toggles steps, long-press opens the knob menu.
     Debugging: `adb forward tcp:9555 localabstract:webview_devtools_remote_<pid>` exposes the WebView over CDP.
     versionCode 2 / versionName 0.2.0. Plug-ins are desktop-only (the hosts are Linux / Wine processes); the Android
     app has everything else. Tested in Electron with touch emulation and on the emulator; not yet on a physical device.
   - Running x86 plug-ins on the phone itself (Box64 + Wine + a glibc userland, like Winlator) was not built; the owner
     chose a plug-in server over Wi-Fi instead (4f).

### 4f. Plug-in server over Wi-Fi (2026-11-16)
   - Desktop (`tools/share.js`, started by desktop/main.js, settings in userData/share.json `{on, port, code}`): off by
     default. When on, an HTTP server on 0.0.0.0:47800 answers `GET /share.json?code=` (which hosts run; host tokens
     never leave the computer) and WebSocket `/clap`, `/win`, `/win32?code=`, which it pipes to the loopback host after
     rewriting the handshake (host token in, Origin header out; the hosts still accept only 127.0.0.1 pages). 6-digit
     pairing code, timing-safe compare, 10 wrong codes a minute lock an address out for a minute; CORS `*`; TCP_NODELAY.
     The app's own server has `/share-config.json` (GET / PUT `{on, port, newCode}`) for Settings › Plug-ins.
   - Client (`audio/bridge.js`): `setPlugServer({addr, code, on})` (main.js calls it at start from `prefs.plugServer`),
     `askServer()`, and `hostInfo()` then returns `ws://addr/<host>?code=` URLs with `remote: true`. Plug-in windows open on
     the computer. `engine.reloadPlugins()` re-plugs every slot after connecting / disconnecting.
   - Bridge without SharedArrayBuffer (`createPortBridge`): the Android WebView is not cross-origin isolated, so the
     worklet and the worker talk over a MessageChannel; same wire protocol. Remote hosts use 256-frame blocks, start at
     3072 frames (learned per kind 'remote' in `lw.pluglat`), grow to 16384, and drop the oldest audio when a burst
     leaves more than twice the target buffered.
   - Android: `capacitor.config.json` has `allowMixedContent` and `server.cleartext`, the manifest
     `usesCleartextTraffic` (the page is https://localhost, the server plain http / ws on the LAN).
   - Tested: from the LAN address with Node (wrong code 403, scan, audio round trip ~1 ms); end to end on the emulator
     (server 10.0.2.2): Undae Test Sine, Surge XT (VST3) and FB-3300 (Windows, Wine) all played, peaks ~0.5, buffer
     settling at 107-181 ms with 4-6 early dropouts (emulator with software rendering on the same machine). Local
     plug-ins on the desktop unchanged (21 / 32 ms, no dropouts). Not yet tried on a physical phone over real Wi-Fi.

5. UI polish: the owner reported misaligned/uneven elements. Fixed so far: step-grid spacing, mixer fader/meter
   layout, and the top bar (one row from 761px up; labels, then the meter, then Tap drop out as it narrows).

Branding: `art/` holds the owner's two source images. `tools/cutout.py` (Pillow only) cuts them to transparent PNGs:
`app/icons/icon-{512,192,64}.png` (initials, app icon) and `app/img/wordmark.png` (top bar).

## Native edition (C++ / JUCE), started 2026-11-16

The owner wants maximum performance, no JavaScript runtime and full OS integration, so the app is being rewritten
natively in `cpp/` (see `cpp/README.md` for building and layout). Chosen: C++20 with JUCE 8.0.15 (in
`cpp/third_party/JUCE`; AGPLv3 or the JUCE licence, to be decided by the owner before distribution). The Electron app stays
the working version until the native one catches up; both read the same `.undae.json` files.

Plan, in phases:
1. Engine, project loading, offline render, real-time playback (done, see below).
2. Editors: top bar, rack, Piano Roll, playlist, mixer (JUCE components, touch-friendly from the start).
3. Everything else: instrument and Sound Design panels, Audio Lab (audio to MIDI, key detection), library, settings,
   hotkeys, MIDI input and links, recording, export formats (WAV / MP3 / Opus), templates, backups.
4. Plug-ins in-process: CLAP (port native/host.c), VST3 / LV2 (JUCE's hosting), Windows plug-ins through the existing
   Wine host but over shared memory instead of a WebSocket; plug-in windows embedded. The Wi-Fi sharing protocol stays.
5. Android (JUCE builds for Android; the touch layout from phase 2).

Phase 1 results:
- The engine ports the browser engine closely: Web Audio's biquad formulas (Q in dB for low / high-pass), the equal-power
  panner with its mono / stereo cases (a mono strip is 3 dB down at centre, as in the browser), channel counts at each
  node, PeriodicWave normalisation, the synth's envelope maths, the drum kit generators, swing, song clips with slips,
  Piano Roll loops, choke / cut groups, mono / poly voice stealing, slide notes, per-note pan / fine / release / cutoff
  / resonance, sends with loop refusal, solo rules, width / phase, the peak controller's side chain, automation of bus /
  channel level and pan, effect parameters and master pitch, audio clips with WSOLA stretch.
- `undae-render` on the demo song vs the browser export (both 48 kHz): same length (23.37 s), overall RMS -19.21 vs
  -18.95 dBFS, loudness envelope correlation 0.992 (median difference 0.02 dB), every band from 20 Hz to 16 kHz within
  0.5 dB. Renders at 16x real time, including building the kit.
- The app (`Undae Sonicae`, 8 MB) plays through ALSA (PipeWire's) or JACK: demo song, 44.1 kHz / 512-frame buffer, no
  xruns, average callback 2.2 ms, worst 7.5 ms of an 11.6 ms budget.
- Deliberate differences: the phaser's feedback works (in the browser its delay-less loop is muted, so its wet signal is
  silent); the reverb's random impulse differs per build (as it does per page load in the browser); the compressor is a
  model of DynamicsCompressorNode (soft knee, 6 ms look-ahead with a peak window, the spec's make-up gain), not
  Chromium's exact kernel; wave shapers run without 2x oversampling; the reverb convolution has 512 samples of latency,
  absorbed by its pre-delay.
- Not yet: plug-ins (slots pass audio through), the editors, live input from MIDI / the keyboard in the window (the engine
  has noteOn / noteOff), recording, packaging (AppImage / .deb). Performance work left: voice pools instead of
  allocating on the audio thread, SIMD in the oscillators.

Phase 2 (2026-11-16): the editors.
- Window: top bar (File menu: new / open / save / save as / export WAV, Play, PAT / SONG, BPM, pattern list and "+",
  position, voices, master meter, undo / redo, audio device) over two decks with tabs (Rack, Piano Roll, Song, Mixer),
  split draggable. Keys: Space, Ctrl+O / S / Shift+S / N / Z / Shift+Z / Y, F5 Song, F6 Rack, F7 Piano Roll, F9 Mixer.
  Unsaved changes are asked about on quit. 40 px controls and 36 px menu rows throughout (touch, for phase 5).
- Rack: mute, solo, level, pan, mixer bus, name (right-click: rename, colour, clone, delete, sound); steps for the pattern
  length (Beats), paint by dragging, right-click clears, the playing step outlined; Swing; "+ Channel" (synth, keys, any
  kit sound). Piano Roll: draw / move / resize / delete, box select (Ctrl+drag), Shift+click to add to the selection,
  arrows transpose (Ctrl: octave), Ctrl+A, Delete, snap, wheel / Ctrl+wheel / pinch, velocity lane, keyboard that plays,
  scrolls to the channel's notes. Song: pattern picker (right-click: rename / clone / delete), place / move / resize /
  delete clips (pattern and audio), note previews in clips, ruler sets the start point, snap, "+ Track", zoom.
  Mixer: strips with meter (-12 / -6 dB zones), fader, pan, mute / solo, send knobs; right-click for sends, master feed,
  rename, mono, phase, delete; "+" adds an insert bus (right-click: a send bus with a reverb); effects panel with on / off,
  order, remove, "+ Add effect" and a knob or list per parameter (the peak controller lists the buses).
- Editing model (`Doc`): views change `doc.p`, then `live()` (while dragging: a snapshot goes to the engine) or `commit()`
  (undo step, engine, views). The engine takes a same-structure snapshot on its next block without locking
  (`std::atomic<std::shared_ptr<Project>>`); a structural change (channels, buses, effects, routing) builds a new graph
  on the UI thread and swaps it in, the old one is freed on the UI thread (`collectGarbage`).
- Saving writes the app's JSON with every field the engine knows merged into the file as read, matched by id, so fields
  the native edition does not use yet (key, arrangements, markers, plug-in states...) survive. Checked: the demo saved
  by the native edition is the same JSON as the original, and renders sample-identically.
- The reverb now uses its own uniformly partitioned FFT convolution (512 samples, absorbed by the pre-delay) instead of
  JUCE's, whose background loading made renders differ run to run: exports are now repeatable byte for byte. Renders
  seed the random generator. Still within 0.01 dB (median) of the browser export.
- `undae-selftest` passes 12 / 12. Real-time: average callback 1.3 ms of 11.6, no xruns; a single slow first callback
  (~15 ms) remains (allocation on the audio thread when voices start: voice pools are the fix).
- Not in phase 2: automation lanes in the playlist, audio clip waveforms, Piano Roll tools (quantize, chords, slide,
  lanes other than velocity), mixer effect presets, the instrument / sound design panels (phase 3).

Phase 3, part 1 (2026-11-16): playing in, instruments, library, recording, exports.
- Live input: every MIDI input is opened and plays the current channel (note on / off with velocity, pitch bend over the
  channel's range, mod wheel into the synth's "wheel" source, sustain pedal holding notes until it comes up). The typing
  keyboard plays too (Z S X D C V G B H N J M and Q 2 W 3 E R 5 T 6 Y 7 U I 9 O 0 P, from C3; `,` / `.` shift octaves).
  Engine: `pitchBend`, `modWheel`, `sustain`, `previewSample`, voices got `setBend` / `setWheel`.
- Record (the ● button): notes played while playing land in the current pattern where they were played (engine
  `setRecording` / `takeRecorded`, drained by the window's timer, one undo step per batch); right-click for "Record audio
  from the input": the input goes through a FIFO into a take that becomes a WAV inside the project and an audio clip at
  the start point, on the first free track.
- Instrument tab: the whole synth (15 presets from presets.js, three oscillators with morph, unison / drift, sub, noise,
  FM, ring, two filters with routing, shaper, four envelopes, three LFOs with sync / fade, the 8-slot mod matrix, macros,
  mode / glide / voices / velocity / gain / bend range) or the sampler (sound, root, tune, fine, start, attack, release,
  choke, gate, reverse, cut, with its waveform).
- Library tab: the kit and imported sounds, click to hear, double-click or "Add as channel", "Put on the playlist",
  "Import audio files…" (WAV / AIFF / FLAC / Ogg / MP3); dropping audio files on the window adds a sampler channel each.
  Imports are stored inside the project (`Project::embedded`, saved as base64 like the browser app does). The sample
  bank is copy-on-write (`std::atomic<std::shared_ptr<map>>`) so the audio thread never sees a half-updated map.
- File menu: export the song as WAV (24-bit), FLAC (24-bit) or Ogg Vorbis (quality 8), the pattern ×4 as WAV; autosave
  every 2 minutes while unsaved (`~/.config/undae-sonicae/native/autosave.undae.json`, "Reopen the autosaved session")
  and a dated backup every 10 minutes, the last 20 kept ("Open the backups folder").
- Tests: `undae-selftest` 16 / 16 (adds: recorded note at the right tick and length, sustain release, audio import saved
  in the project and rendered from the reloaded file). In the app: FLAC and Ogg exports within 0.2 dB of the browser
  render; a 3.6 s audio take recorded from PipeWire's input and placed on the playlist.
- Still to do in phase 3: Audio Lab (audio to MIDI, key detection), Sound Design, settings (theme, hotkeys, MIDI device
  choice, latency), templates, MP3 / Opus export (JUCE has no encoder for either: needs LAME / libopus), playlist
  automation lanes, Piano Roll tools.

Phase 3, part 2 (2026-11-16): Audio Lab, Sound Design, settings, templates, Piano Roll tools, MP3.
- `engine/Analysis` ports analysis.js: YIN melody transcription (median smoothing, re-attacks, octave-blip clean-up),
  harmonic-sum chord transcription, Krumhansl-Schmuckler key (with Camelot, confidence, alternatives), onset-autocorrelation
  tempo, toTicks / snapToScale. Self-test: the demo's lead line comes back as exactly its six notes, each within a 16th;
  the song's key is A minor; the tempo 92.2 BPM.
- Audio Lab tab: load a file or pick any sound / take, play it, detect key and tempo (background thread, progress), use
  the key / tempo for the project, convert to notes (single line or chords, gate, shortest note, quantize, start at bar
  1, snap to the key, replace or add, into the selected channel or a new synth channel; the pattern grows to fit).
  The project now has a key (`Project::keyRoot / keyScale`, the app's `key` field).
- `engine/SoundDesign` + Sound Design tab: design.js's one-shot builder (swept tone with FM, filtered noise, a sample
  layer, drive, filter sweep, attack / fade, normalise, built-in effects with a tail), its ten presets, "edit" any
  library sound, play on change, save to the library / + Rack / onto the selected sampler (WAV inside the project).
- Settings tab: JUCE's device selector (output / input / rate / buffer / MIDI inputs; the setup is remembered in
  `~/.config/undae-sonicae/native/settings.xml`), project name / time signature / key / scale / swing grid, typing
  keyboard on / off, autosave on / off, start-up (empty, last session, a template), templates (save, new from, delete;
  also File > New from template), stored in `.../native/templates/`.
- Piano Roll: Tools menu (quantize starts / and lengths, legato, chop, glue, transpose an octave, reverse, humanize,
  lengths x2 / /2, duplicate to the right, fit to the key, detect the key from the notes, select all, delete) and
  "Snap to key" for drawing and moving.
- MP3 export (320 kbps) through LAME loaded at run time (`dlopen("libmp3lame.so.0")`): no build dependency; the menu
  says when it is missing. Checked: valid MPEG-1 layer III, level within 0.25 dB of the browser render (the decoded
  file is ~50 ms longer: MP3 encoder delay). Opus is not done (libopus is installed, libopusenc / an Ogg muxer is not).
- `undae-selftest` 21 / 21.
- Left for later: live tuner / mic-driven instrument in the Audio Lab, the Library's sample folders, theme choice and a
  hotkey editor, playlist automation lanes, Opus export.

Phase 4 (2026-11-16): plug-ins in the native edition.
- `engine/PluginHost`: the native app drives the same host processes as the Electron app (`native/bin/undae-host` for
  CLAP and Linux VST3, `undae-vsthost.exe` / `undae-vsthost32.exe` under Wine), started with fork / exec (stdin kept open:
  the hosts exit when it closes; Wine prefix `~/.local/share/undae-sonicae/wine`). A small WebSocket client (masked
  frames, TCP_NODELAY, a reader thread per instance) speaks native/PROTOCOL.md. Audio is synchronous: the audio thread
  sends a block and waits for its answer (8 ms timeout live, 10 s offline; a late block is dropped). Measured round
  trips: 0.04-0.12 ms on average, worst ~1 ms, against a 2.67 ms block, so no bridge buffer: plug-ins add no latency.
- Engine: plug-in instances for the live graph live in a pool keyed "ch:<channel>" / "fx:<bus>:<effect>" (`syncPlugins`
  loads new ones on a thread, drops removed ones; a change of plug-in is part of the structure key). Instruments get notes
  stamped with their sample frame (note-off sent ahead with its own frame, as the browser does); live bend / mod wheel go
  as MIDI. Parameter values in the slot (`v<id>`) are pushed when they differ from what the plug-in last heard (moves in
  the plug-in's own window update that record, so nothing echoes back). Stop sends `panic`. Exports load their own fresh
  instances (state and values restored) and wait for every block, so the live ones keep playing.
- UI: rack "+ Channel" > Plug-in instrument; mixer "+ Add effect" > Plug-in; the chooser scans all hosts once (cached,
  "Rescan"). The Instrument tab (plug-in channels) and the mixer's effect panel show the plug-in: status, "Open window"
  (its own window, from its host), "Change plug-in…", a filter and knobs for up to 64 matching parameters. The window
  polls plug-in events: parameter moves in the plug-in's window go into the project, a "dirty" event reads the state into
  the slot (so it saves). Channel plug-in slots are juce::var objects shared by undo snapshots: `Doc::editablePlug` copies
  before writing.
- Tests: `undae-plugtest` loads and runs the test synth, Surge XT (VST3, 2855 parameters), the test gain, FB-3300 and
  TAL BitCrusher (Windows, Wine), all PASS. `cpp/examples/first-light-plugins.undae.json` (Surge XT on the lead, FB-3300
  on the bass, TAL BitCrusher on the drum bus) exports at 3.4x real time with each instrument audible; live playback of it
  averaged 4.9 ms of an 11.6 ms callback with no xruns, one 38 ms callback while the Wine plug-ins came up.
- Not yet: LV2; plug-in windows embedded inside the app's window (they open as their own windows); automation of plug-in
  parameters in the playlist; plug-ins in the Sound Design effect chain; a host crash is reported only as silence (no
  automatic restart).

Phase 5 (2026-11-16): packages, Android, touch, plug-ins over Wi-Fi.
- Linux packages: `sh cpp/package.sh` -> `dist/undae-sonicae-native_0.2.0_amd64.deb` (4.1 MB: /opt/undae-sonicae-native
  with the plug-in hosts beside the app, `undae-sonicae-native` command, menu entry, icons) and
  `dist/Undae Sonicae Native-0.2.0.AppImage` (3.9 MB). Both AppImages now use the static type-2 runtime from
  github.com/AppImage/type2-runtime (fetched with the owner's OK to `~/.cache/undae-sonicae/runtime-x86_64`, sha256
  617aa773...8a46), so they run without libfuse2 (checked on this machine, which has none). `cpp/package.sh` uses it when
  present (else electron-builder's older runtime); `npm run dist:linux` ends with `tools/appimage-runtime.sh`, which swaps it
  into the Electron AppImage (contents unchanged, launcher replaced). Checked: the AppImage plays the plug-in demo, finding its bundled hosts.
- Android: `cpp/UndaeSonicae.jucer` + the Projucer (built from third_party/JUCE with JUCE_BUILD_EXTRAS) generate
  `cpp/Builds/Android` (Gradle 9.1, AGP 8.13, NDK 28.1 fetched by Gradle, arm64-v8a and x86_64, package
  `app.undaesonicae.studio`; "native" is a Java keyword). Code changes for it: `AtomicShared` (libc++ has no
  std::atomic<std::shared_ptr>), no host processes on Android, full screen within the safe area, microphone permission
  asked at run time, the wordmark built in (BinaryData). Release APK (both ABIs, -O3, zipaligned, signed with the local
  debug key so it sideloads): `builds/undae-sonicae-native-0.2.0-android.apk` (31.6 MB). The debug build was the one
  run on the API 37 emulator: it starts, taps open menus, a synth channel was added and notes tapped into the Piano Roll.
- Narrow screens (< 1000 px wide, i.e. phones): the top bar on two rows, one deck, tabs that scroll sideways (never
  narrower than 96 px).
- Touch (`app/Touch.h`, a global mouse listener): a long press = right-click (menus, erase a note, delete a clip; on
  buttons that open menus on right-click, `wantsMenu()` makes the long press open the menu and the release does not click);
  two fingers pinch the Piano Roll and playlist (`pinchZoom` component property -> `mouseMagnify`).
- Plug-in server in the native client: `HostSet::useServer(addr, code)` reads the desktop app's `/share.json` and points
  every plug-in at `ws://addr/<host>?code=` (the Electron app's gateway, tools/share.js). Remote instances are pipelined
  (about 8 blocks buffered, re-filled after a gap) instead of awaited. Settings › Plug-in server; remembered and
  reconnected at start. Checked: `undae-plugtest --server <ip> <code>` against the Electron app's sharing played Surge XT
  with sound 21 ms after the note.
- Not yet: an on-screen piano keyboard in the native app, a signed release APK, testing on a physical phone, the native
  app as a sharing server itself (it is a client; the Electron app shares).

## Things worth knowing about the code

- `app/js/state.js` holds the project model, prefs (`localStorage` keys `lw.prefs`, `lw.project`, `lw.last`,
  `lw.backups`, `lw.bg`), undo history and migrations (`migrate`, `fixSynth`).
- `app/js/util.js` exports `BAR`, `BEAT` and `SIG` as **live bindings** changed by `setSig()`; never cache them at
  module load.
- `app/js/audio/engine.js`: `Graph` builds the mixer for both the live `AudioContext` and the offline one used for
  export. The graph is rebuilt only when `sigOf(P)` changes (buses, effect ids, send targets); otherwise `sync()`.
- User samples and recordings live in IndexedDB (`undae-sonicae`, stores `samples` and `folders`).
- A project file (`*.undae.json`) embeds the samples it uses.
- Testing tip: do not call `setProject(demoProject())` in a browser that holds the owner's work; it replaces the
  autosave. Snapshot `S.p` first and restore it.
- If a dev server caches aggressively, fetch changed files with `{ cache: 'reload' }` before reloading the page.

## Known limits

One tempo and one time signature per project; automation covers level, pan and effect parameters only (not synth
parameters and not plug-in parameters); recorded audio is stretched to tempo but not re-pitched; the floating deck is a panel inside the window,
not a separate OS window.

### Native 0.3.0 (2026-11-17): the old app's look and features in the native edition
The owner found the native UI less pleasing and less complete than the Electron one, and asked for every feature from the
chat to be there with the same behaviour. The native UI was rebuilt view by view from app/js:
- `cpp/src/app/Theme.h` ports app.css: the eight themes (Slate default), 13 px text, 30 px controls (40 on touch), the
  widgets (.btn, .tb, .seg, .mini, .busno, pill tabs with the old icons, conic knobs with the right-click menu, menus,
  toast, Flow for .vbar rows). `Cards.h` is the .cards / .card / .krow grid with line graphs and waveforms.
- `Keys.h`: the whole hotkey table (keys.js), rebindable in Settings › Hotkeys (stored as "keymap"), scoped to the focused
  view. Window: brand/project menu, transport group (play, record dialog, metronome, PAT/SONG), tempo (scroll, Tap, time
  signature menu), pattern box and its ⋯ menu, B:S:T / M:S:CS position, master meter, master pitch, CPU / RAM / voices
  with history (/proc of the app and its host processes), undo / redo, MIDI devices, on-screen keyboard (docked and
  resizable, or floating). One to four decks plus a floating deck (Settings › General), views swap between decks.
- Engine: metronome and one-bar count-in, panic, wait-for-input and pause (window), input monitoring per bus, an input tap
  (Audio Lab), per-bus waveform readout, input levels, plug-in effects in Sound Design renders. Model: channel groups,
  bus input / arm / monitor / stereo, clip groups and audio clip colours, markers, MIDI links, rack group filter, and
  arrangements (the current one in `song`, the others as JSON) — all saved in the same file format.
- Rack, Piano Roll, Song, Mixer, Instrument, Sound Design, Audio Lab, Library and Settings follow their app/js versions
  (tools, stamps, lanes, dialogs, picker, arrangements, automation lanes incl. plug-in parameters, strip presets, input menu,
  render armed buses, mic recording and live pitch in the Lab, sample folders in the Library, eight settings tabs).
- Recording: notes (with record quantize), audio as one take per armed bus (stacked per pass with loop record), and
  automation of knobs and faders while the song plays. MIDI export / import, MIDI controller links (learn, multiple).
- Not ported: Ogg Opus export (MP3 / FLAC / Ogg Vorbis are there), sharing this computer's plug-ins (the Electron app
  does that), the growing waveform of a take while it records, the swung-step offset drawing in the rack.
- Android 0.3.0 (versionCode 4): `builds/undae-sonicae-native-0.3.0-android.apk` (release, both ABIs, signed with the
  local debug key). Found while testing it on the API 37 emulator, and fixed for every platform:
  - Text: a few literals with ·, › or curly quotes were built as `juce::String(const char*)`, which reads bytes as ASCII
    (garbled text, and a JUCE assertion logged on Android). They now go through `u8()`. Note that JUCE 8 reads a
    `std::string` as UTF-8, so `juce::String(c.name)` is fine; only `const char*` literals need `u8()`.
  - Idle CPU: the top bar was repainted whole 30 times a second (a full core on the emulator, where JUCE draws in
    software). Now only the position, meter and stats areas repaint, and only when they change. Mixer strips stop
    repainting once a silent bus's waveform has scrolled flat.
  - The engine rests when stopped: after a second of silence with no voices, clips, held keys, monitored inputs or
    plug-ins (`Graph::canRest`), `Engine::process` writes zeros without running the effects, until a note, preview or
    play. Desktop idle went from about 29% (UI) + 20% (audio) of a core to 3% + 2%; playback renders the same.
    `--play-at <seconds>` starts playback later (used to check play after the engine has rested).
  - Long popup menus: a finger drag picked the item it ended on. JUCE is patched (`cpp/patches/juce-android.patch`,
    applied in `third_party/JUCE`) so a touch drag scrolls the menu and picks nothing; a tap still picks. Re-apply the
    patch after updating JUCE.
  - Drawing on Android: JUCE paints the canvas clip, and with hardware acceleration Android gives the whole view for
    any invalidate, so every change was a full-screen software repaint (measured with `--bench-paint`: about 210 ms
    at 1680x2087 with the background glow, 75 ms without). Android now attaches a `juce::OpenGLContext` to the main
    component (module juce_opengl in the .jucer only; desktop is unchanged): GPU drawing, and only changed areas are
    repainted into its cached frame. The same patch file makes JUCE textures work on GLES 3.0 (the emulator; JUCE 8
    asks a texture's size with glGetTexLevelParameteriv, GLES 3.1+), else images and gradients are not drawn.
  - The "rack is empty" hint wraps to the visible width (phones).
  - Checked on the API 37 emulator (x86_64, GLES 3.0 via SwiftShader; it refuses this laptop's Intel GPU): starts with
    no assertions, menus scroll by drag and pick by tap, a synth plays with the mixer scope and meter moving, and the
    picture survives going to the home screen and back and rotating. Not measured: GPU frame cost on a real phone.
  - Quicker test builds: `./gradlew assembleRelease -Pandroid.injected.build.abi=x86_64` builds only the emulator's ABI
    (output in app/build/intermediates/apk/..., marked test-only: `adb install -t`).
- Phone layout fixes (still 0.3.0), from a report on a real phone in portrait:
  - Settings tabs wrap into as many rows as they need (each row stretched to the width) instead of running off the right
    edge (Plug-ins and Project were unreachable on a phone upright, and in narrow floating decks on desktop).
  - The rack's channel column can be resized: a grip at its right edge (a line with a ◂▸ handle) drags it narrower or wider;
    a tap or click on the grip switches compact / full. Narrowing drops the controls one by one (piano roll, bus, pan,
    solo, volume, ⋯) and then shrinks the name; the mute LED and the name always stay (long press / right-click on the
    name has the channel menu). The width is kept per orientation (`rackHeadTall` / `rackHeadWide` in the settings file);
    until set, the column is full when that leaves room for about 8 steps, else compact (a phone upright gets compact).
  - Top bar: when it does not fit on one row it now takes two - logo, transport and tempo; then pattern, position, pitch
    and the undo / redo / MIDI / keyboard box - instead of flowing and leaving that box alone at the right of a third row
    (an empty strip to its left on phones). Before wrapping, and to make two rows fit, `layoutTop` squeezes in steps
    (`tight` 1-3: gaps, play button, tempo box, Tap, logo, record / metronome buttons); a landscape phone can then keep
    one row. Spare width in a wrapped row goes to the position readout (whole 1:01:00) and the pattern box. The narrowest
    screens get three rows (logo + transport; tempo + tools; pattern). Wide windows are laid out as before.
  - A deck's view-tab row scrolls the open view's tab into sight when it lays out (after a restart in portrait, the
    Settings tab could be off the edge of the row).
  - Rotation: Android can report the new notch and system-bar insets after the window's new size, and then the window
    size does not change again, so the app kept the landscape safe area in portrait (blank strips left and right). The
    window now lays the app out again whenever the screen's details change (`parentSizeChanged`), and Main checks the
    safe area twice a second on Android in case that is missed. Emulator: `adb shell cmd overlay enable-exclusive
    --category com.android.internal.display.cutout.emulation.hole` (a notch) and `...systemui.navbar.threebutton`
    (side nav bar in landscape) give it real side insets; `adb emu rotate` turns it.

## Native edition on Windows (handover 2026-11-17)

The owner wants a native Windows version of the C++ / JUCE edition (0.3.0), built and tested on their Windows desktop.
The Linux build (deb + AppImage) and the Android build stay the main targets on the Ubuntu laptop. Nothing Windows-
specific has been built yet; the points below are what a read of the code shows needs doing.

### Getting the project

The project came on the USB drive "TINGS" (FAT32) as `undae-sonicae/`. Copy that folder to a local disk first (for
example `C:\dev\undae-sonicae`) and work there: building on a FAT32 stick is slow, and it cannot hold files over 4 GB.

- The folder is a git repository (branch `main`). Build output, `node_modules`, the packages in `dist/` and `builds/`,
  and JUCE are not tracked (see `.gitignore`). Files are stored as they are on Linux. `.gitattributes` keeps the shell
  scripts LF, and keeps the JUCE patch and the Projucer output (`cpp/JuceLibraryCode`) byte-exact. Do not let an editor
  or a renormalise change their line endings: the patch must match JUCE's CRLF `juce_PopupMenu.cpp`.
- JUCE 8.0.15 comes separately as `JUCE-8.0.15-patched.zip` at the top of the stick. It is one file because copying its
  4,450 files to the stick one by one was very slow. Extract it into `cpp\third_party\` so that
  `cpp\third_party\JUCE\CMakeLists.txt` exists, with Explorer's "Extract All" or
  `tar -xf E:\JUCE-8.0.15-patched.zip -C cpp\third_party`.
- The zip holds a shallow clone of the release tag with the two local changes applied: `git -C cpp/third_party/JUCE
  status` lists `juce_PopupMenu.cpp` and `juce_OpenGLTexture.cpp` as modified. Without the zip, run
  `git clone --depth 1 --branch 8.0.15 https://github.com/juce-framework/JUCE cpp/third_party/JUCE`, then
  `git -C cpp/third_party/JUCE apply ../../patches/juce-android.patch`.
- FAT32 kept no executable bits, so the project's copy on the stick and JUCE's clone in the zip both have
  `core.fileMode false`.

### Tools

Visual Studio 2022 (or its Build Tools) with the "Desktop development with C++" workload, which includes CMake and
Ninja, plus Git for Windows. JUCE 8 builds with MSVC or clang-cl only; MinGW is no longer supported for JUCE. From an
"x64 Native Tools Command Prompt for VS 2022", in the project folder:

```bat
cmake -S cpp -B cpp/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build
```

Programs land in `cpp\build\<target>_artefacts\Release\` as on Linux (see `cpp/README.md` for what each one does).

### What has to change for Windows

Keep the Linux and Android paths as they are, since they cannot be tested on Windows: add Windows branches under
`#if JUCE_WINDOWS` (or `_WIN32` in the plain C hosts) rather than rewriting shared code.

1. `cpp/CMakeLists.txt`
   - `-Wall -Wextra -Wno-unused-parameter` and `-O3` (on `us_engine`) are gcc / clang flags. Put them under
     `if(NOT MSVC)`. MSVC reads `-Wall` as `/Wall`, which floods the build with JUCE warnings; use `/W3` there.
   - `JUCE_JACK=1 JUCE_ALSA=1` on the app are Linux-only. WASAPI and DirectSound are on by default on Windows. ASIO needs
     Steinberg's ASIO SDK (its own licence), then `JUCE_ASIO=1` and the SDK's include path. It is optional; WASAPI
     exclusive mode already gives low latency.
2. `cpp/src/engine/PluginHost.cpp` uses POSIX throughout:
   - `pipe` / `fork` / `execvp` / `kill` / `waitpid` to start the host processes and read the JSON line they print.
   - BSD sockets with `MSG_NOSIGNAL`, `poll`, `SIGPIPE` and `timeval` timeouts for the WebSocket client.
   - On Windows, use `CreateProcess` with a redirected stdout and an environment block (the hosts read
     `UNDAE_HOST_TOKEN` and `UNDAE_SERIAL`), and `TerminateProcess` to stop them. Use Winsock: `WSAStartup`,
     `closesocket`, send flags 0, `WSAPoll`, and `SO_RCVTIMEO` / `SO_SNDTIMEO` as a DWORD of milliseconds.
   - `HostSet::start` runs the Windows VST hosts through Wine (`which wine`, `WINEPREFIX`). On Windows, run
     `undae-vsthost.exe` directly; the 32-bit one runs under WoW64.
   - The Windows plug-in folder is `~/.vst-win` on Linux. On Windows use the standard ones: `C:\Program Files\Common
     Files\VST3`, `C:\Program Files\VSTPlugins`, `C:\Program Files\Steinberg\VSTPlugins`, `C:\Program Files\Common
     Files\VST2`, and the `(x86)` versions for the 32-bit host.
   - `findBinDir` looks for `undae-host` with no extension.
   - With no host processes, plug-in slots pass audio through, so the app is usable before this is finished.
3. `native/win/vsthost.c` (VST2 + VST3, 64- and 32-bit) is a Windows program already, but it is built on Linux against
   Wine's DLLs, and `dos_path()` turns every path into `Z:\...` (Wine's view of the Linux root). On Windows, paths are
   already native: pass them through when they start with a drive letter or `\\`, and check the folder scan in
   `vst3win.h` the same way. It is freestanding C (no C library of its own; `winshim.h` declares the Win32 calls it
   uses). Rebuild it on Windows with clang or MinGW-w64 gcc (fine for this C program; only JUCE dropped MinGW), or
   with MSVC if `winshim.h` agrees with it. The prebuilt `native/bin/undae-vsthost*.exe` import kernel32, user32,
   ws2_32, advapi32 and msvcrt by name and might start on Windows as they are, but they will not find plug-ins until
   `dos_path` is fixed.
4. `native/host.c`, the CLAP host (plus Linux VST3), is POSIX and X11 throughout: `dlopen`, `eventfd`, `prctl`,
   pthreads and X11 windows for plug-in editors. For CLAP on Windows, either port it (`LoadLibrary`, Winsock, Win32
   windows) or teach `vsthost.c` to load `.clap` files, since it already has the Win32 window and message loop. Phase
   4 of the plan prefers in-process hosting with JUCE in the long run; that is a separate, larger job.
5. `cpp/src/engine/Mp3.cpp` loads LAME with `dlopen("libmp3lame.so.0")`. On Windows use `juce::DynamicLibrary` or
   `LoadLibraryA` with `libmp3lame.dll` (a LAME DLL shipped next to the exe; LGPL). Without it, the MP3 export items
   are greyed out ("needs LAME, libmp3lame"); WAV, FLAC and Ogg work.
6. `cpp/src/app/AppMain.cpp:28-60`: the CPU / RAM / voices readout reads `/proc/<pid>/stat` for the app and its host
   processes (Linux and Android only). On Windows use `GetProcessTimes` and `GetProcessMemoryInfo` (psapi), or leave
   the readout empty at first.
7. Settings already go to `juce::File::userApplicationDataDirectory` (`%APPDATA%\undae-sonicae` on Windows), and file
   dialogs use JUCE's special locations, so no path work is expected there. JUCE 8 draws with Direct2D on Windows;
   the OpenGL context is attached on Android only.
8. `isTouch()` (`cpp/src/app/Theme.h`) is true on Android / iOS only. A Windows tablet gets the desktop sizes unless
   that changes; the Settings density option still helps.
9. Packaging: `cpp/package.sh` is Linux-only. For Windows, make an installer (Inno Setup is the simplest) that installs
   to Program Files with a Start menu entry. Ship `Undae Sonicae.exe`, `wordmark.png` if it is used next to the exe
   (`AppMain.cpp:487`), the hosts from `native/bin` (rebuilt), and `libmp3lame.dll`, and register `.undae.json` to
   open with the app. A zip of the Release folder is enough for a first test.

### Checking it

- `undae-selftest cpp\examples\first-light.undae.json` must end with "all passed".
- `undae-render cpp\examples\first-light.undae.json out.wav`, then `undae-compare` against a render made on Linux:
  it should be the same to within rounding.
- `"Undae Sonicae.exe" cpp\examples\first-light.undae.json --play --quit-after 8` prints the audio callback stats;
  `--snapshot shot.png` and `--size WxH` help check the layout.
- Plug-ins: `undae-plugtest` once the hosts run, then a real VST3 instrument and effect in the app (editor window open,
  automation, save and reload).

### Windows status (2026-11-17, branch `windows`)

Built on Windows 11 with VS 2022 Build Tools (MSVC 19.44): `cpp\build-win.bat` sets up the MSVC environment, puts VS's
own CMake and Ninja ahead of any MinGW copies on PATH, then configures and builds `cpp\build`. All six targets build.

Done:
- `cpp/CMakeLists.txt`: gcc flags and `-O3` under `if(NOT MSVC)`, `/W3`, `/utf-8`, `_USE_MATH_DEFINES`, `NOMINMAX`;
  JACK / ALSA on Linux only; `ws2_32` and `psapi` linked on Windows.
- `PluginHost.cpp`: Winsock shim (`closeSock`, `setTimeout`, `WSAStartup`), `CreateProcessW` spawn with piped
  stdin/stdout and an environment block, `TerminateProcess` stop. The VST hosts run directly (no Wine) and scan the
  standard VST folders, passed one per line in `HostEndpoint::folder` (scanPlugins splits it). `findBinDir` looks for
  `undae-vsthost.exe` on Windows and stops at the drive root. Every VST3 counts as a Windows slot there.
- `Mp3.cpp`: `LoadLibrary` of `libmp3lame.dll` (next to the exe first).
- `AppMain.cpp`: CPU / RAM readout through Toolhelp, `GetProcessTimes` and `GetProcessMemoryInfo`.
- Linux / Android code paths are unchanged (not compiled here: build and self-test on the laptop after merging).

Checked: `undae-selftest` all passed; `undae-render` renders First Light (23.4 s in 1.9 s); the app plays it through
WASAPI (average callback 0.76 ms of a 10 ms budget) and the `--snapshot` layout matches Linux.

Windows VST hosts (item 3, done the same day):
- `native/build-win.ps1` builds both hosts on Windows with MinGW-w64 (WinLibs): the same freestanding compile as
  `build.sh`, plus `-mno-stack-arg-probe`, linked through import libraries that dlltool makes from
  `native/win/imports.def.txt`. Real Windows' kernel32 / user32 forward some functions to ntdll, which ld cannot link
  to directly as it does with Wine's DLLs. The 32-bit `.def` lines are `Sleep@4 == Sleep` (names taken from `nm -u`).
- `dos_path()` passes native paths (`C:\...`, `\\server\...`) through; `base_name()` takes either slash; `scan_dir`
  builds child paths with the folder's own separator; a `.vst3` folder without `Contents\` (an old flat bundle,
  TAL-Vocoder-2) loads the DLL inside it directly.
- The VST2 scan no longer unloads each DLL: on real Windows, freeing Klanghelm MJUCjr wedged the next LoadLibrary
  (u-he Protoverb). Scanned DLLs stay loaded until the host exits.
- The rebuilt `native/bin/undae-vsthost*.exe` are committed. On the laptop, run `native/build.sh` after merging so the
  Wine builds come back from the same source (not tested under Wine).
- Plug-in folders on Windows: the standard VST2 / VST3 folders plus `UNDAE_VST_PATH` (folders separated by `;`).
  There is no Settings entry for this yet.
- `undae-plugtest "Name" ...` tests the named plug-ins (instrument or effect as the scan says) instead of the usual set.
  Passed on Windows: PG-8X, TAL-BassLine (32-bit), mda Delay (32), dblue Glitch (32), MJUCjr, Protoverb, Graillon 2
  (VST3), iZotope Vocal Doubler (VST3), at 0.1-0.3 ms per block. FBVC and TAL-Vocoder-2 load and run but stay silent
  with the test's plain tone (vocoders need a carrier).
- Some plug-ins write files into the working directory (`MIDI Programs\`) or the Desktop (u-he logs).

Not done yet: `undae-compare` against a Linux render; the CLAP host (item 4); shipping `libmp3lame.dll`; a Settings
entry for plug-in folders; the installer (item 9).

### Bringing the work back to Linux

Commit on a branch named `windows`. To carry it back by USB, push that branch to the copy on the stick, for example
`git push E:\undae-sonicae windows`. The stick's copy has `main` checked out, so pushing another branch to it is allowed.
FAT32 records no file owner, so Git for Windows may call that repository "dubious ownership". Its message gives the
`git config --global --add safe.directory ...` line that allows it.
On the laptop, run `git fetch /run/media/izpiritu/TINGS/undae-sonicae windows`, then merge `FETCH_HEAD` and check that
the Linux build and the self-test still pass. The laptop repository has the stick as the remote `tings`, so
`git fetch tings windows` also works there.

## Built-in instruments and effects to replace unlicensed plug-ins (plan, 2026-11-17)

Several commercial plug-ins in `E:\VST` need licences the owner no longer has. The aim is original built-in
equivalents that do the same *kinds* of job, not copies. Rules for everything built here: original names, parameter
sets, presets and UI (the app's own Theme and generic knob panel); no product names or trademarks in the app, no
look-alike layouts, no copied factory presets. The survey below used file and folder names only, plus public knowledge
of each product category; nothing was disassembled, decompiled or extracted.

### Survey: what the third-party plug-ins are for (by category)

A category marked (?) is a guess from the name alone.

| Category | `E:\VST` | FL Studio `Plugins\Fruity` |
|---|---|---|
| Synths | Serum, Zebra2, Zebralette, TyrellN6, Podolski (installer only), PG-8X, TAL-BassLine, TAL-Elek7ro-II, monofury, nabla, mps (?), oxid (?), Orpheus (?), expo (?) | 3x Osc, Sytrus, Harmor, Harmless, Morphine, PoiZone, Sakura, Sawer, SimSynth, Toxic Biohazard, Transistor Bass, Wasp/XT, DX10, BooBass, Plucked!, GMS, Kepler/Exo, FL Keys, Minisynth, BeepMap, Autogun |
| Samplers / ROMplers / granular | LABS, Nexus (content only), Ribs, grainstrain | DirectWave, FLEX, Slicex, Fruity Slicer/2, Fruity Granulizer, Soundfont Player, Wave Traveller |
| Drum machines | MT-PowerDrumKit, mda BeatBox | FPC, Drumaxx, Fruity DrumSynth Live, BassDrum, Fruit Kick, Drumpad |
| EQ | SSL Fusion Violet EQ, SSL 4K B / 4K G, SSL Channel Strip 2, Harrison 32Classic Channel Strip / 32Bus, SSL X-DynEQ (dynamic) | Fruity Parametric EQ/2, 7 Band EQ, EQUO, Bass Boost, Low Lifter |
| Compressor / limiter / transient | SSL X-Comp, Bus Compressor 2, Fusion HF Compressor, RoughRider3, MJUCjr, Harrison DynoMite, Couture (transients), mda Dynamics / Limiter / MultiBand | Fruity Compressor, Limiter, Multiband Compressor, Maximus, Soundgoodizer, Transient Processor |
| Saturation / tape / tube / exciter | TubeSaturatorVintage, SSL X-Saturator, Fusion Vintage Drive, Fusion Transformer, ThrillseekerXTC, La Petite Excite, mda Overdrive / Bandisto / Combo, Frohmage, Bee-Happy (?), SL84 (?) | Fruity Blood Overdrive, Fast Dist, Soft Clipper, WaveShaper, Distructor, Hardcore, Emphasis / Emphasizer |
| Reverb | ZRev, SSL GateVerb | Fruity Reeverb/2, Luxeverb, Fruity Convolver |
| Delay | SSL X-Delay, X-Echo, mda Delay / DubDelay | Fruity Delay/2/3, Delay Bank, Multiband Delay |
| Chorus / phaser / flanger / rotary | Acon Multiply, azurite (?), Harrison StunningPhaser, mda ThruZero / Detune / Leslie / RingMod / RoundPan | Fruity Chorus, Flanger, Flangus, Phaser, Vintage Chorus / Phaser, Hyper Chorus, Frequency Shifter, PanOMatic |
| Vocoder / talkbox | TAL-Vocoder-2, fbvc, mda Vocoder / Talkbox / VocInput | Fruity Vocoder, Vocodex |
| Pitch correction / formant / doubler | Auto-Tune 8.1, Graillon 2, Vocal Doubler, LALA (?), Martin Eastwood Duet (?), mda RePsycho! / Shepard | Newtone, Pitcher, Pitch Shifter, Newtime |
| De-esser | SSL DeEss, Harrison De-Esser, SPITFISH, mda De-ess | – (EQ / multiband instead) |
| Gate | Harrison SpectralGate, SSL GateVerb (gated reverb) | Fruity Mute 2 (?), Gross Beat (volume gating) |
| Stereo imaging | SSL Fusion Stereo Image, mda Stereo / Image / Splitter | Fruity Stereo Enhancer, Stereo Shaper, Spreader, Center, Phase Inverter |
| Glitch / stutter / tape stop | dblue Glitch / TapeStop / Stretch, Fracture, Hysteresis | Gross Beat, Fruity Scratcher, Effector |
| Bitcrusher / lo-fi | dblue Crusher, SSL Digicrush, mda Degrade, iZotope Vinyl | Fruity Squeeze (?), Effector |
| Analyzers / meters | JScope, mda TestTone | Wave Candy, Spectroman, dB Meter, Tuner |
| Channel strips / vocal chains | SSL Vocalstrip 2, Sourcerer (?), Harrison VocalFlow / VocalIntensityProcessor | – |
| Note tools (MIDI) | hypercyclic (arpeggiator), 1208 SEQ (step sequencer) | VFX Sequencer, Key Mapper, Keyboard Splitter |

The owner is right that most are effects: roughly 70 effects against 20 instruments in `E:\VST`.

### What the native engine already has

- Effects (`cpp/src/engine/Effects.cpp`, listed for the UI in `cpp/src/app/Doc.cpp` `fxSpecs()`): 3-band EQ (low
  shelf / peak / high shelf), filter (LP / HP / BP / notch), compressor and limiter (soft knee, 6 ms look-ahead),
  reverb (convolution with a generated noise tail), delay (feedback with tone), chorus, phaser (4 all-pass stages),
  distortion (tanh), bitcrush (bit depth only), peak controller (side-chain ducking, whole or one band).
- Instruments: the subtractive / FM synth (`Voices.cpp`: 3 oscillators with morph and unison, sub, noise, FM, ring,
  two filters, shaper, 4 envelopes, 3 LFOs, 8-slot mod matrix, glide / slides), the sample player, the synthesised
  808-style kit and keys sound (`Samples.cpp`), Sound Design one-shots (`SoundDesign.cpp`), YIN pitch tracking
  (`Analysis.cpp`, offline only).
- Adding a built-in effect takes three pieces: a class in `Effects.cpp`, a line in `makeEffect()`, and an `FxSpec`
  in `fxSpecs()`. The Mixer's "+ Add effect" menu, Sound Design, Settings and automation pick it up from there. The
  side-chain path (`sideSource()` / `side()`) already exists for the peak controller, so a vocoder or a keyed gate can
  take its carrier / key from another bus.
- Caution: the Electron edition's `makeFx` (`app/js/audio/fx.js`) fails on an effect type it does not know. Before a
  project that uses a new effect is opened there, either port the effect or make unknown types pass audio through.

### Gaps, in the order to fill them

Working names are original and can change. DSP is standard published technique only.

1. **Parametric EQ ("Contour")**: 6 bands (high-pass, low shelf, 3 peaks, high shelf, low-pass), RBJ cookbook
   biquads, HP / LP at 12 or 24 dB/oct (cascaded), output trim. Covers the channel-strip EQs and the multi-band EQs;
   the biggest single gap. A live response curve comes later with the analyser (11).
2. **De-esser ("Sibilant")**: detector on a high-passed copy, fast envelope, gain reduction applied to the whole
   signal or only above the split (complementary crossover), listen switch for the detector band.
3. **Gate / expander ("Hush")**: open / close thresholds (hysteresis), attack / hold / release, range (an expander
   when less than full), detector high-pass, optional key from another bus (the existing side-chain path).
4. **Saturation ("Ember")**: modes tape / tube / transformer / clip, oversampled waveshapers: asymmetric biased tanh
   (tube, even harmonics), soft clip with pre / de-emphasis and a gentle low bump (tape), cubic with low-frequency
   weighting (transformer); drive, bias, tone, mix, output. Exciter mode: saturate only a high-passed band and add it.
5. **Transient shaper ("Snap")**: fast and slow envelope followers; their difference drives the gain for attack and
   sustain, each +-100 %.
6. **Stereo imager ("Breadth")**: mid / side width 0-200 %, mono below a crossover (bass mono), balance, optional
   Haas widening for mono sources, correlation readout.
7. **Algorithmic reverb ("Vault")**: 8-line feedback delay network, Hadamard mixing matrix, mutually prime delay
   lengths, slow delay modulation, input diffusion all-passes, one-pole damping per line for decay vs frequency;
   modes room / hall / plate / gated. Lighter and more controllable than the noise convolution, which stays.
8. **Delay upgrade ("Echo Line")**: tempo sync (the effect needs the project tempo: add it to `Effect` next to
   `frame`), ping-pong, stereo offset, filtered / saturated feedback, wow / flutter, wet ducked by the dry.
9. **Glitch / stutter / tape stop ("Splinter")**: a tempo-synced capture buffer; per step it repeats a slice (1/4 to
   1/64), reverses it, slows it to a stop (variable-rate read) or gates it. Needs tempo and song position (as 8).
10. **Vocoder ("Choir of Wires")**: filter bank of 8-32 log-spaced band-pass pairs, envelope followers on the
    modulator, carrier from another bus via the side chain or a built-in saw / noise carrier, an unvoiced (noise)
    band for consonants, band shift for formant effects.
11. **Analyser and meters**: FFT spectrum (Hann, 4096, log frequency axis), oscilloscope, true-peak and LUFS meter
    (ITU-R BS.1770 K-weighting: momentary / short-term / integrated), phase correlation, a tuner from the existing
    YIN code. More UI than DSP.
12. **Pitch correction and formant ("True North")**: real-time YIN (the existing `yin()` made incremental) ->
    nearest note in the project key / a chosen scale -> retune-speed smoothing -> PSOLA pitch shift on the detected
    periods (keeps formants); formant shift by resampling the grains. The hardest item (latency, warble on breathy
    input). A doubler (two or three short delayed copies a few cents apart, panned) comes nearly free from the shifter.
13. **Multiband compressor ("Tiers")**: 3 bands, Linkwitz-Riley 4th-order crossovers, the existing `Compressor` per
    band. A dynamic-EQ mode later.
14. **Modulation extras**: flanger (short modulated delay with feedback, through-zero option), ring modulator,
    frequency shifter (Hilbert transform from an IIR all-pass pair), rotary speaker (amplitude + Doppler on two rotors
    with a crossover), tempo-synced auto-pan / tremolo.
15. **Lo-fi ("Dust")**: sample-rate reduction (sample and hold), bit depth with dither, generated vinyl noise /
    crackle, wow, band-limiting.

Instruments (the synth already covers most subtractive and FM jobs; these fill what it cannot):

- **Wavetable mode for the synth**: original generated tables (additive spectra, formant sweeps, PWM / sync series),
  frame position on the mod matrix. Covers the wavetable synths.
- **Granular sampler**: grains from a sample (position, size, density, spray, pitch, reverse), Hann window, as a
  channel type. Covers the granular / texture instruments.
- **Multi-sample instrument**: key / velocity zones from imported samples, round robin, loop points; a drum-pad
  variant maps pads to samples with choke groups (the engine already has choke groups).
- **Plucked string / physical models**: Karplus-Strong with fractional-delay tuning and a damping filter; modal
  resonators for mallets and bars.
- **Arpeggiator / step sequencer** on synth channels, note-level before the voices.
- **Original preset banks** for the existing synth in the styles vintage polys and acid basslines are used for
  (a style, not a product), written from scratch.

Suggested order: 1 (EQ) and 2 (de-esser) together, as they share filters and sit on nearly every vocal; then 4, 3,
6, 7; then the tempo-aware 8 and 9; then 10 and 12.

### Built so far (2026-11-17, Windows)

The first four from the list are built-in effects now. Each is in `Effects.cpp` (`makeEffect`), in the Mixer's
"+ Add effect" menu and Sound Design (`fxSpecs()` in `Doc.cpp`), and checked by `undae-selftest` (27 / 27 on Windows):
- **Parametric EQ** (`peq`, item 1): low cut off / 12 / 24 dB/oct, low shelf, three peaks with width, high shelf, high
  cut, output. Check: +12 dB band at 1 kHz measures +12.0 dB; 24 dB/oct cut at 200 Hz leaves 50 Hz at -48 dB.
- **De-esser** (`deess`, item 2): detector above "From Hz", 4:1 above the threshold up to "Range" dB; turns down only
  the highs (4th-order Linkwitz-Riley split) or the whole signal; Listen plays the high band. Check: a loud 10 kHz tone
  is turned down 10 dB, 200 Hz is untouched.
- **Saturator** (`sat`, item 4): tube / tape / transformer / clip / exciter, 4x oversampled with integer latency (the dry
  path is delayed to match), level-compensated. Check: tube mode gives a 2nd harmonic at -18 dB with the level held.
- **Vault reverb** (`fdn`, item 7): hall / room / plate / gated, 8-line FDN. Check: a 2 s decay falls 33 dB in one
  second (30 in theory). Every saturator and reverb mode at full settings stays finite and bounded.

Not done yet: these effects are not in the Electron edition (see the caution above: opening such a project there fails
until `fx.js` gets them or a pass-through); no response-curve or gain-reduction display yet; not tried by ear in the app
window. Next in the order: gate (3), transient shaper (5), stereo imager (6), then the tempo-aware delay and glitch.

Second batch (2026-11-17, Windows), self-test 30 / 30:
- **Gate** (`gate`, item 3): open / close thresholds (hysteresis), attack, hold, release, range (80 dB = shut, less = an
  expander), detector high-pass, optional "Key from" bus through the side-chain path (the engine's graph key includes
  it, as for the peak controller). Check: a quiet passage is shut 66 dB, a loud key bus holds a quiet signal open.
- **Transients** (`trans`, item 5): attack and sustain -100..+100 %, output, mix. Check: attack +100 % lifts the start
  of a hit, sustain -100 % lowers its tail about 10 dB.
- **Stereo width** (`imager`, item 6): width 0-200 %, bass mono below a frequency (LR4 high-pass on the side), Haas
  widening, balance. Check: the 60 Hz side drops 32 dB with bass mono at 150 Hz, 2 kHz untouched; width 0 % is mono.
Next in the order: the tempo-aware delay (8) and glitch / stutter (9), then the vocoder (10).

Third batch (2026-11-17, Windows), self-test 33 / 33. Effects now know the tempo and the song position: `Effect::bpm`
and `Effect::beat` (quarter notes since the song start, -1 when stopped), set by `Graph::render` from the transport
(`Graph::tickAt` / `timeAt` / `rolling`; exports roll from tick 0).
- **Tempo delay** (`echo`, item 8): note lengths from 1/16 to a bar or free ms, stereo or ping-pong, right side
  later by up to 50 %, low / high cut and drive inside the feedback, wow / flutter, ducking; time changes glide. Checks:
  1/8 at 120 BPM echoes at exactly 12000 and 24000 samples; in an export of a 120 BPM project a 1/4 echo lands 24000
  samples after the kick.
- **Glitch** (`glitch`, item 9): every 1/16-1/2 note a seeded choice (the same on every play and export) of pass,
  repeat (loop the last 1/64-1/8), reverse (the previous step backwards), tape stop, chop; "Pattern" picks another
  sequence. Stopped, it runs on its own clock at the project tempo. Checks: a repeat step is periodic in the slice, a
  reverse step is the previous step backwards, two renders are identical.
Next in the order: the vocoder (10), the analyser and meters (11), pitch correction (12).

- **Vocoder** (`vocoder`, item 10), self-test 34 / 34: goes on the voice's bus; carrier from another bus through the
  side chain (in the graph key, like the gate) or a built-in buzz at a chosen note; 4-32 bands between Low and High Hz
  (4th-order band-passes), band width, formant shift +-1 octave, attack / release, hiss for sibilance above 5 kHz.
  Checks: a 1 kHz voice brings out the buzz harmonic at 1046 Hz 26 dB over 392 Hz; silence in is silence out (-65 dB);
  a side-chained carrier keeps the voice's 3 kHz 47 dB over 440 Hz. A single-tone voice comes out about 9 dB quieter
  than it went in (Output makes up for it); real speech spreads over more bands.
Next: the analyser and meters (11), pitch correction (12), multiband compressor (13).

- **Meters** (`meters`, item 11), self-test 35 / 35: a pass-through slot whose mixer card shows a live spectrum (4096-point
  FFT, log axis, slow fall), an oscilloscope (zero-crossing trigger, auto zoom) or a tuner (`pitchOf`), plus momentary /
  short-term / integrated LUFS (BS.1770 K-weighting from the standard's analogue prototypes; gated integration),
  true peak (4x oversampled, held) and correlation; click the readings to restart. The audio thread publishes into a
  `MeterTap` found by slot id (`Effect::attach`, live graphs only, so exports do not disturb it); the panel is
  `app/MetersPanel.h`. Check: a -20 dBFS 1 kHz sine on both channels reads -19.99 LUFS (reference -20), -20.0 dBTP,
  correlation +1 / -1, and the tuner hears 440 Hz as note 69.01. Seen in the app window: spectrum and scope views of
  First Light with the readings. Not in Sound Design (nothing to watch there).
Next: pitch correction (12), multiband compressor (13), modulation extras (14), lo-fi (15), then the instruments.

- **Pitch correct** (`pitchfix`, item 12), self-test 36 / 36: real-time YIN on a 4x-decimated copy every 128 samples
  (median of three against octave jumps), correction toward the nearest note of the key gliding over Speed ms (0 =
  instant), TD-PSOLA pitch shift with two-period Hann grains (formants kept), Formant +-6 semitones by stretching the
  grains, Amount, Lowest Hz (sets the latency), Mix (dry delayed to match). Key and Scale default to the song's
  (`Effect::keyRoot` / `keyScale`, set from the project like the tempo). Corrections are limited to +-6 semitones.
  Latency is lowest period x (1 + 1.42) + 4 samples: about 27 ms at the default 90 Hz; the engine does not compensate
  effect latency yet, so the corrected track sits that much late (lower it by raising Lowest Hz for higher voices).
  Checks: A4 +39 cents comes out at A4 (+2.6 cents), C#4 +30 cents in C major goes to D4, Amount 0 leaves pitch and
  level alone, level ripple 0.7 dB on a steady tone, 72x real time. Not yet tried on a real recorded voice.
Next: multiband compressor (13), modulation extras (14), lo-fi (15), then the instruments.

- **Multiband comp** (`multiband`, item 13), self-test 37 / 37: three bands from 4th-order Linkwitz-Riley crossovers
  (the low band also through the upper crossover's pair, so the bands sum flat), per-band threshold / ratio / gain,
  shared attack / release, 6 dB soft knee, stereo-linked, Listen low / mid / high, output. No dry blend (the crossovers
  turn the phase). Checks: flat with ratio 1, the low band at 10:1 pulls 80 Hz down 18 dB while 1 kHz is untouched,
  Listen mid drops 60 Hz 42 dB. No gain-reduction display yet.
Next: modulation extras (14), lo-fi (15), then the instruments.

- **Modulation extras** (item 14), self-test 38 / 38:
  - **Flanger** (`flanger`): swept short delay, free or tempo-locked sweep, feedback +-0.95, stereo phase offset. Check:
    a fixed 5 ms comb notches 100 Hz by 41 dB and lifts 200 Hz by 3.0 dB.
  - **Ring mod** (`ringmod`): band-limited sine / triangle / square carrier, frequency wobble. Check: 300 Hz x 1 kHz
    gives 700 and 1300 Hz with no 300 Hz left.
  - **Freq shifter** (`fshift`): IIR Hilbert pair (Niemitalo's coefficients) and single-sideband shift +-1000 Hz plus
    fine, right side the same or opposite. Check: 1000 Hz shifted +100 comes out at 1100 Hz, the mirror at 900 Hz -49 dB.
  - **Rotary speaker** (`rotary`): horn / drum split at 800 Hz, slow / fast / stop with spin-up inertia, amplitude,
    Doppler and mic swing, drive. Check: it moves the level and the two sides differ.
  - **Auto-pan** (`autopan`): tremolo or pan, sine / triangle / soft square, note lengths locked to the song or free.
    Check: at 1/4 and depth 1 the level dips 67 dB halfway between beats; pan swings the sides 63 dB apart.
Next: lo-fi (15), then the instruments.

- **Lo-fi** (`lofi`, item 15), self-test 39 / 39: sample and hold down to 1 kHz (no anti-alias filter, on purpose),
  2-16 bits with optional TPDF dither, wow and flutter, generated surface noise and crackle (seeded, repeatable),
  24 dB/oct low / high cut, narrowing toward mono. Checks: 8 kHz sample and hold leaves the 7 kHz image of a 1 kHz tone;
  4 bits leaves a near-full sine 26.6 dB above its quantising noise (theory 26); crackle on silence is identical on
  two runs; the 3 kHz high cut takes 10 kHz down 47 dB.

All fifteen effect gaps from the list are now filled: 19 new built-in effect types, 30 with the original eleven.
Next: the instruments (wavetable mode for the synth, granular sampler, multi-sample instrument, plucked string /
modal models, arpeggiator / step sequencer, original preset banks).

- **Wavetable mode for the synth** (self-test 40 / 40): seven original tables of 8 frames each, generated from formulas
  in `Dsp.cpp` (`waveSets()`, frames "wt:<id>:<k>", built at start-up in `WaveTable::warmUp`, about 0.16 s): Saw to
  square, Opening (low-pass sweep), Pulse width, Vowel (moving formant), Sync (hard-sync series), Fold (wave folding),
  Glass (sparse partials). Picking "Table: ..." as an oscillator's wave turns Morph into Position, which the mod
  matrix's "morph" destination moves; the panel preview draws the current frame. Five original presets use them:
  Vowel Pad, Sync Lead, Fold Bass, Glass Keys, PWM Strings. Checks: the pulse table has no 2nd harmonic at position 0
  and a strong one at 1; the opening table's 10th harmonic rises from -83 to -21 dB; every wavetable preset plays.
- **Granular sampler** (self-test 41 / 41): a mode of any sample channel (`Channel::gran`, saved as "gran" in the
  channel), switched on in the sampler panel's Granular card: position, scan (moves through the sample by itself),
  grain size, density, jitter, pitch spray, stereo spread, reverse probability. `GrainVoice` in `Voices.cpp` lays
  Hann-windowed grains while the note is held and fades them over the channel's release; randomness comes from the
  engine's seeded generator, so exports repeat. Checks: grains of a 440 Hz sample stay on 440 Hz (8 dB over the
  neighbouring semitones: overlapping grains spread the pitch a little) and follow the keyboard an octave up (32 dB);
  silence 0.15 s after the release; the settings survive save and reload.
- **Multi-sample zones** (self-test 42 / 42): `Channel::zones` (saved as "zones": sample, lo / hi note, root, vlo / vhi
  velocity, gain dB). When present they replace the channel's single sample; zones holding the same note and velocity
  take turns (round robin, `rr` in the channel strip). Zone root and gain reach the voice through `NoteOpts`, so
  granular mode works per zone too. Sampler panel: Zones card with + Zone, per-zone sample / range / root / velocity /
  gain / remove, and "Auto-map imported..." (roots from note names in the sample names, else by pitch detection;
  ranges split halfway between roots). Check, rendered through the engine: C4 plays from the A3 zone, E5 from the A5
  zone, nothing crosses; two overlapping zones alternate (12.0 dB apart as set). Seen in the app window.
- **Physical models** (self-test 43 / 43): `SynthP::pm` (saved as "model" in the synth), a source beside the
  oscillators, through the filters and amp envelope. "Plucked string": Karplus-Strong with a filtered noise burst,
  pluck-position comb, fractional delay tuned for the loop filter's delay, per-pass gain for the set decay. "Struck":
  two-pole modal resonators at bar / glass / wood / bell ratios, higher modes dying faster unless Bright, hardness
  sets the strike length and the upper modes' level. Synth panel card "Physical model"; seven original presets: Nylon
  Pluck, Steel Pluck, Pluck Bass, Vibes, Glass Mallet, Wood Block, Tower Bell. Checks: the string plays note 57.001 and
  falls 18 dB in 0.5 s at a 2 s decay (15 in theory, the loop filter takes a little more); the bar rings at 1 and
  2.756 times the note; every model preset plays.
- **Arpeggiator / step sequencer** (self-test 44 / 44): `Channel::arp` (saved as "arp"), for every instrument type
  (synth, sampler, plug-in). Notes reaching an arpeggiated channel are held in the strip (`Strip::held`, up to 32;
  live keys until let go) instead of sounding; `Graph::runArps` plays them on a 1/32 to 1/4 grid locked to the song
  (its own clock when stopped): up, down, up and down, as played, random (seeded by the step number, so repeatable),
  over 1-4 octaves, gate length, and a 1-16 step on / off pattern for rhythm. Card "Arpeggiator" at the bottom of every
  instrument panel. Check, rendered through the engine: a held C major chord at 120 BPM plays C E G C on the 1/16 grid,
  silent between notes at gate 50 %, and a switched-off step rests.
- **Synth preset bank** (self-test 45 / 45): 15 more original patches named for what they do (Brass Section, Poly
  Strings, Square Lead, Chip Lead, Electric Piano, Wobble Bass, Stab Chord, Warm Bass, Hollow Pad, Saw Stack, Breathy
  Flute, Sweep Pad, Arp Pluck, Deep Sine Bass, Choir Swell): 42 synth presets in all. The wavetable "Glass Keys" was
  renamed "Crystal Keys" (the name was taken). Check: names unique and every preset sounds, finite and not clipping.
- **Studio Kit** (self-test 46 / 46): 14 synthesised acoustic-flavoured drums in `Samples.cpp` ("kit2:*", group
  "Studio Kit": kick, snare, side stick, hand clap, closed / pedal / open hat, ride, crash, floor / rack / high tom,
  shaker, tambourine), appended after the other generators so the 808 kit's random draws are unchanged (First Light
  renders identically to the build before this session: every band +0.00 dB). They appear in the Library and the
  sampler's sound list. Check: all present and normalised, names unique across kits, kick energy 98 % below 200 Hz,
  closed hat 90 % above 5 kHz.

Every item on the plan above is now built. Second round (gaps the survey also shows, being worked on next): doubler,
pitch shifter, auto-filter (envelope / LFO wah), amp simulator, sub-bass enhancer, dynamic EQ, effect chains
(channel-strip and mastering presets that add several slots at once), impulse-response loading for convolution.

- **Second-round effects** (self-test 47 / 47), all in `Effects.cpp` and the Mixer menu:
  - **Pitch shifter** (`pitchshift`): delay-line shifter with two sine-windowed heads, +-12 semitones and cents.
    Check: +12 puts a 440 Hz tone at 880 Hz, 67 dB over what is left at 440.
  - **Doubler** (`doubler`): two copies a few cents up / down, delayed and wandering, spread left and right. Check:
    the sides decorrelate (0.71) with the level kept.
  - **Auto-filter** (`autofilter`): zero-delay-feedback state-variable filter (low / band / high), cutoff moved by an
    envelope follower and an LFO (free or locked to the song), in octaves, drive. Check: a loud tone opens it 39 dB
    more than a quiet one.
  - **Amp** (`amp`): two 4x-oversampled asymmetric gain stages, bass / mid / treble, small / large / bright cabinet or
    none. Check: a strong 3rd harmonic, 10 kHz well down through the cabinet.
  - **Sub enhancer** (`subgen`): octave divider on the low band, smoothed and enveloped, plus a low shelf. Check: adds
    50 Hz under a 100 Hz bass.
  - **Dynamic EQ** (`dyneq`): two peaking bands moving with the level in their own band. Check: a loud 1 kHz goes down
    the full 12 dB set, a quiet one and 100 Hz are untouched.
- **Effect chains and menu categories** (self-test 48 / 48): `fxChains()` / `chainSlots()` in `Doc.cpp`, eight
  original ready-made strips that add several slots at once (Vocal strip, Drum bus, Bass, Mastering, Lo-fi beat,
  Guitar, Radio voice, Wide pad), under "Chains" in the Mixer's "+ Add effect" menu. The effects are now grouped into
  sub-menus by `fxCategories()` (EQ & filter, Dynamics, Distortion & colour, Space, Modulation, Pitch & voice,
  Stereo & creative, Meters). Check: every chain slot and setting exists in the effect specs, every effect has a
  category, and each chain processes a mix cleanly.
- **IR reverb** (`irverb`, self-test 49 / 49): convolution with any imported sound (option list "@samples" in the
  Mixer: the project's imported audio). Effects now get the sample bank (`Effect::samples`, set by `Graph::build`
  before `set()`, exports included). The impulse is resampled to the engine rate (up to 12 s), normalised to unit
  energy and run through the same partitioned convolver as the generated reverb; the dry path is delayed by its 512
  samples; pre-delay, low / high cut and gain on the wet. Not offered in Sound Design (no bank there). Check: a click
  through an imported click-plus-echo impulse comes out 512 samples late with the echo exactly 0.1 s after, at 0.53 of
  the level (0.5 set; resampling 44.1 to 48 kHz spreads the peak a little).

Second round done. Next ideas: a beat slicer for sample channels (cut a loop at its hits, one slice per key, and
write the pattern that replays it).

## Linux catch-up list (for later; work is Windows-only for now)

The owner is working on Windows only for now (Android work is fine from Windows: the SDK, Android Studio, the emulator
and JDK 25 are installed there). Nothing below needs doing until they go back to the laptop. Keep this
list current as Windows work adds to it.

State when the list was started: the laptop repository is `~/Desktop/New Folder/undae-sonicae`, its `main` at 753c525
(the first four built-in effects), built with JUCE from the stick's zip, self-test 27 / 27 on Linux. It has the stick as
the remote `tings`. Everything after 753c525 on `windows` is untested on Linux.

1. **Bring the code over**: `git fetch tings windows`, then `git merge FETCH_HEAD` in the laptop repository (or copy
   the branch some other way if the stick is not used).
2. **Build and self-test**: `cmake --build cpp/build`, then
   `cpp/build/undae-selftest_artefacts/Release/undae-selftest cpp/examples/first-light.undae.json`. Every check must
   pass (49 after the IR reverb; more as items are added). Fix any gcc-only warnings or errors the MSVC build did not show.
3. **Wine VST hosts**: run `native/build.sh` so the Linux / Wine builds of `undae-vsthost*` come from the same source as
   the Windows ones (the Windows changes to `dos_path`, `scan_dir` and the unload-free VST2 scan were never tried under
   Wine). Then `undae-plugtest` with the usual Wine plug-ins.
4. **Electron edition**: `app/js/audio/fx.js` `makeFx` fails on effect types it does not know. Either make unknown
   types pass audio through, or port the built-in effects (so far: `peq`, `deess`, `sat`, `fdn`, `gate`, `trans`,
   `imager`, `echo`, `glitch`, `vocoder`, `meters`, `pitchfix`, `multiband`, `flanger`, `ringmod`, `fshift`, `rotary`,
   `autopan`, `lofi`, `pitchshift`, `doubler`, `autofilter`, `amp`, `subgen`, `dyneq`, `irverb`). The synth's "wt:" wave names and the five wavetable presets also need porting to
   `synth.js` (unknown waves there fall back to sine), the sampler's "gran" and "zones" settings, the synth's "model" source, and the channel "arp" to the Electron engine. Until then, do not open projects that use them in the Electron app.
5. **Compare renders**: `undae-compare` between a Windows render and a Linux render of First Light (never done).
6. **Android**: can be done from Windows (not Linux-only). Rebuild the APK and check that the new effects run there
   (the saturator uses JUCE's `dsp::Oversampling`; check CPU on the phone).
7. **Branches**: the laptop's `main` was fast-forwarded to `windows`. Decide whether `main` simply follows `windows`
   from now on, and push the result back to wherever the canonical copy lives.
8. **Anything else added on Windows**: each later Windows change that touches platform code (`PluginHost.cpp`,
   `Mp3.cpp`, `AppMain.cpp` CPU / RAM readout, CMake) gets a line here.
