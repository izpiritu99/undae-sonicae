# Undae Sonicae — native edition (C++ / JUCE)

A native rewrite of the app, without Electron or any JavaScript runtime. It reads the same project files
(`.undae.json`, saved from the current app with File › Save project file) and aims to sound the same.

## Build

JUCE 8.0.15 goes in `third_party/JUCE` (not tracked by git): clone the `8.0.15` tag there, then apply the two local
changes kept in `patches/juce-android.patch` (`git -C cpp/third_party/JUCE apply ../../patches/juce-android.patch`): a
finger drag scrolls a long popup menu instead of picking the item it lands on, and OpenGL textures work on GLES 3.0
phones (JUCE asks the texture size with a GLES 3.1 call). Re-apply it after updating JUCE.

| Platform | Command | Needs |
|---|---|---|
| Windows | `cpp\build-win.bat` (sets up MSVC and uses Visual Studio's own CMake and Ninja, ahead of any MinGW copies) | Visual Studio 2022 / Build Tools, "Desktop development with C++" |
| macOS | `cpp/build-mac.sh`: universal build in `cpp/build-mac`, self-test, LAME and the plug-in host put inside the app, ad hoc signature, `cpp/dist/*.dmg` | Xcode command line tools, `brew install cmake ninja lame` |
| Linux | `cmake -S cpp -B cpp/build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build cpp/build` | gcc / clang (C++20), CMake ≥ 3.22, Ninja, ALSA, JACK, FreeType, fontconfig, X11 and GL headers |

GitHub Actions (`.github/workflows/mac.yml`) runs the macOS build on an Intel Mac on every push to `windows` or `main`:
build, self-test, a Surge XT plug-in test (AU and VST3), a start-up screenshot, and the `.dmg` as a download.

The Windows plug-in hosts (`native/bin/undae-vsthost*.exe`) are built with `native/build-win.ps1` (MinGW-w64), the
Linux ones with `native/build.sh`.

Outputs in `cpp/build/<target>_artefacts/Release/`:

| Program | What it does |
|---|---|
| `Undae Sonicae` | the app. `"Undae Sonicae" song.undae.json --play` opens and plays a project; `--quit-after 8` prints audio stats and exits; `--snapshot shot.png --size 1360x860` saves a picture of the window (for tests) |
| `undae-render` | renders a project to WAV: `undae-render song.undae.json out.wav [--pattern NAME] [--loops N] [--rate 48000] [--bits 24]` |
| `undae-compare` | compares two renders (level, stereo balance, loudness envelope, spectrum per band) |
| `undae-plugtest` | starts the plug-in hosts, scans, then loads each test plug-in and runs audio through it; `undae-plugtest "PG-8X" "Surge XT@au"` tests the named ones (`@format` picks AU / VST3 / VST2), `--scan-json` prints the scan |
| `undae-selftest` | the engine and editing path without a window: every built-in effect, instrument and edit, undo / redo, save and reload, identical re-render: `undae-selftest cpp/examples/first-light.undae.json` must end with "all passed" |
| `undae-pluginhost` | the plug-in host on JUCE (Audio Units, VST3, VST2), used by the app on macOS. Elsewhere `UNDAE_PLUGIN_HOST=juce` makes the app use it instead of the usual hosts (for testing it) |

Plug-in folders: on Windows the standard VST2 / VST3 folders, on macOS `/Library/Audio/Plug-Ins` and
`~/Library/Audio/Plug-Ins` (Components, VST3, VST); `UNDAE_VST_PATH` (or `UNDAE_PLUGIN_PATH`) adds folders,
separated by `;`.

## Packages

`sh cpp/package.sh` builds `dist/undae-sonicae-native_<version>_amd64.deb` (installs to `/opt/undae-sonicae-native`,
command `undae-sonicae-native`, menu entry) and `dist/Undae Sonicae Native-<version>.AppImage`. The AppImage uses
the static AppImage runtime in `~/.cache/undae-sonicae/runtime-x86_64` when present (runs without libfuse2), else
electron-builder's cached one (needs libfuse2, or `--appimage-extract-and-run`).

On macOS, `cpp/build-mac.sh` makes the `.dmg`. On Windows there is no installer yet: a zip of the Release folder with
`native\bin` beside it works (Inno Setup is the plan, see HANDOVER.md).

## Android

`cpp/UndaeSonicae.jucer` describes the Android build (JUCE's Projucer is built from `third_party/JUCE` with
`-DJUCE_BUILD_EXTRAS=ON`). `Projucer --resave cpp/UndaeSonicae.jucer` writes `cpp/Builds/Android`, then
`./gradlew assembleRelease` in it (Gradle 9.1, AGP 8.13; Gradle fetches the NDK). Set `JAVA_HOME` to Android Studio's
JBR: `/opt/android-studio/jbr` on Linux, `C:\Program Files\Android\Android Studio\jbr` on Windows (`gradlew.bat`). On
Android there are no plug-in hosts: Settings › Plug-in server uses a computer's plug-ins over Wi-Fi.

## Layout

```
src/engine/   no user interface; shared by everything
  Dsp         Web Audio-equivalent building blocks: biquads (Web Audio formulas, Q in dB for low/high-pass),
              equal-power panner, wave shapers, band-limited wave tables (PeriodicWave normalisation),
              AudioParam-style timelines, the synth's ADSR, delay lines
  Project     the project model, read from the app's JSON (field names and defaults from app/js/state.js)
  Samples     the 808 kit and stock instruments, synthesised as app/js/audio/drums.js does; samples inside project files
  Voices      the synth voice (app/js/audio/synth.js) and the sample voice
  Effects     the mixer effects (app/js/audio/fx.js) and a compressor modelled on DynamicsCompressorNode
  Analysis    pitch tracking, melody / chord transcription, key, tempo (analysis.js)
  SoundDesign the one-shot builder (design.js)       Mp3        MP3 export through LAME loaded at run time
  PluginHost  the plug-in host processes and a WebSocket client for them (synchronous audio)
  Engine      sequencer (collect(), swing, song clips and slips, Piano Roll loops), channel strips, mixer graph
              (inserts, sends, master, solo / mute, width, phase, side chains), automation, audio clips with WSOLA
              time-stretch, offline and real-time rendering
src/app/      the window
  Doc         the open project: undo history, save, and the link to the engine (commit = undo step, live = while dragging)
  Theme       the Slate look, knobs
  RackView    channel rack and step sequencer      RollView   Piano Roll
  SongView    playlist and pattern picker          MixerView  mixer strips, sends, effects with their knobs
  InstView    synth and sampler editors            LibView    kit and imported sounds, audio import
  Presets     the synth presets                    Form       cards of knobs / lists / toggles (Lab, Design, Settings)
  LabView     Audio Lab: key, tempo, audio to notes  DesignView Sound Design
  SettingsView devices, project, playing, saving, start-up, templates
  PlugUI      plug-in chooser and the panel for a loaded plug-in
  Touch       long press = right-click, two-finger pinch zoom (touch screens)
  MetersPanel spectrum, scope, tuner and loudness readings for the Meters effect
  AppMain     top bar (transport, record, BPM, pattern, position, meters, undo, File menu) over up to four decks with
              tabs (and floating decks); MIDI input, typing keyboard, recording, export, autosave, CPU / RAM readout
src/pluginhost/ undae-pluginhost: HostMain (protocol, VST3 / AU through JUCE), Vst2.h (the VST2 interface, no SDK)
src/render/   undae-render, undae-compare, undae-selftest, undae-plugtest
```

## License

JUCE is dual-licensed: AGPLv3, or the JUCE 8 licence (free "Starter" tier below a revenue limit, paid above). Building
and using it yourself is fine either way; pick one before distributing the app.
