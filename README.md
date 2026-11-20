# Undae Sonicae

A pattern-based DAW: a step-sequencer rack, piano roll, song arranger and mixer, with its own synth, sampler and more
than forty built-in effects, plus VST2, VST3 and Audio Unit plug-ins on the desktop. It runs on Windows, macOS, Linux and
Android, and every edition opens the same project files (`.undae.json`).

There are two editions in this repository:

| Edition | Where | Status |
| --- | --- | --- |
| **Native** (C++ / JUCE) | `cpp/` | The main edition, being developed. Windows, macOS, Linux, Android. |
| Web (JavaScript, Web Audio) | `app/`, wrapped by Electron (`desktop/`) and Capacitor (`android/`) | The original. Kept working, but newer features are native-only. |

## Download

Ready-made builds are on the [Releases page](https://github.com/izpiritu99/undae-sonicae/releases): a Windows
installer and portable zip, a macOS `.dmg` (Intel and Apple Silicon), a Linux AppImage and `.deb`, and an Android APK.
Each release lists how to open the files on each system (the Windows and Mac builds are not code-signed, so both systems
ask once).

## Build it yourself

### Native edition

| Platform | How | Needs |
| --- | --- | --- |
| Windows | `cpp\build-win.bat` → `cpp\build\UndaeSonicae_artefacts\Release\Undae Sonicae.exe` | Visual Studio 2022 or its Build Tools ("Desktop development with C++") |
| macOS | `cpp/build-mac.sh` → `cpp/dist/Undae-Sonicae-0.3.0-mac.dmg` (universal: Apple Silicon and Intel, macOS 11+) | Xcode command line tools, `brew install cmake ninja lame` |
| macOS without a Mac | Push to GitHub: `.github/workflows/mac.yml` builds, tests and uploads the `.dmg` on an Intel Mac | A GitHub repository |
| A release | Actions › Release › Run workflow (or `gh workflow run release.yml -f version=0.3.0`): Windows installer and zip, Mac `.dmg`, Linux AppImage and `.deb`, published as release `v<version>`; `platforms=windows` (etc.) rebuilds only some | A GitHub repository |
| Linux | `cmake -S cpp -B cpp/build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build cpp/build`; `sh cpp/package.sh` makes a `.deb` and an AppImage | gcc or clang, CMake, Ninja, JUCE's Linux packages |
| Android | Projucer writes `cpp/Builds/Android` from `cpp/UndaeSonicae.jucer`, then `gradlew assembleRelease` | Android Studio (SDK, NDK, JDK) |

Every build needs JUCE 8.0.15 with the project's patch in `cpp/third_party/JUCE`: it is not in the repository. Clone the
`8.0.15` tag there and apply `cpp/patches/juce-android.patch`, or unzip the patched copy if you have one. Details, and
the command-line tools (`undae-render`, `undae-selftest`, `undae-plugtest`, …), are in [cpp/README.md](cpp/README.md).

Opening a Mac build on another Mac: it is signed ad hoc, not notarised, so macOS blocks it the first time. Open
**System Settings › Privacy & Security** and click **Open Anyway** once.

### Web edition

| Target | Command | Needs |
| --- | --- | --- |
| Browser | `npm start`, then open http://localhost:5173 | Node 18+ |
| Linux desktop (Electron) | `npm install`, then `npm run desktop`; `npm run dist:linux` makes an AppImage and a `.deb` | Node 18+ |
| Android (Capacitor) | `npm install`, `npm run android:init` (once), `npm run android:apk` | Node 18+, JDK 17+, Android SDK |

`app/` is plain ES modules with no build step. After changing it, run `npm run android:sync` before rebuilding the APK.

## What's in it (native edition)

- **Rack**: channel rack and step sequencer, per-step velocity, swing, fill / Euclidean / shift / randomise, pattern
  lengths in beats, row views (steps, notes, waveform).
- **Piano Roll**: draw, select, slice, erase and more; velocity lane; snap with triplets; scale highlight and lock;
  ghost notes; quantise, legato, chop, arpeggiate, strum, humanise; slides; loops.
- **Song**: pattern and audio clips on tracks, automation lanes (draw them or record fader moves).
- **Mixer**: master, insert and send buses with free routing, side chains and the peak controller (ducking);
  per-bus effect chains with bypass and reordering, effect categories and ready-made chains (vocal strip, drum bus,
  mastering and more).
- **Built-in effects**: EQ, parametric and dynamic EQ, filter, auto-filter, compressor, multiband, limiter, gate,
  transient shaper, de-esser, saturator, distortion, amp, lo-fi, bitcrush, reverb, Vault (FDN) reverb, IR reverb,
  delays (plain, tempo, multi-tap), chorus, ensemble, phaser, flanger, rotary speaker, auto-pan, trance gate, ring mod,
  frequency shifter, pitch shifter, pitch correction, doubler, vocoder, glitch, scratch, spectral gate, sub enhancer,
  stereo width, utility, test tone, and meters (spectrum, scope, tuner, LUFS, true peak).
- **Instruments**: a synth with three oscillators, wavetables, physical models (plucked string, struck bar / glass /
  bell), filters, envelopes, LFOs and a mod matrix, and 40+ original presets; a sampler with granular mode, multi-sample
  zones (round robin), a beat slicer and choke groups; an arpeggiator / step sequencer on any instrument.
- **Plug-ins**: VST2 and VST3 on Windows and Linux, Audio Units, VST3 and VST2 on macOS (see Known limits). Each runs
  in a separate host process, so a crashing plug-in cannot take a song down. On Android, Settings › Plug-in server
  uses a computer's plug-ins over Wi-Fi.
- **Sound Design**: build one-shot sounds and send them to the rack. **Audio Lab**: key, tempo, audio to notes.
  **Library**: the 808 and Studio kits, stock sounds, your imports.
- **Recording and MIDI**: audio and MIDI recording, loop record, MIDI keyboards (notes, pitch bend, controllers), an
  on-screen and a typing keyboard, `.mid` import and export.
- **Export**: WAV, FLAC, Ogg Vorbis and MP3 (through LAME), at 16 / 24 / 32-bit and 44.1 to 96 kHz.
- **Workspace**: up to four decks of views, each switchable, and floating decks; 8 themes, custom accent, background
  photo with dimming and deck transparency, interface size, density; rebindable hotkeys; undo / redo, autosave,
  backups, project templates.

## Known limits

- **macOS** is new: it builds and passes its tests on GitHub's Intel Mac, but has not yet been tried by a person on a
  real Mac (sound, recording, plug-in editor windows). Intel-only plug-ins on Apple Silicon run in a second host under
  Rosetta; that path is untested.
- **Windows**: no CLAP plug-ins yet; no ASIO driver (Windows Audio in Exclusive or Low Latency mode is used for low
  delay). A self-built copy needs `libmp3lame.dll` next to the app for MP3 export (the release includes it).
- **Android**: no desktop plug-ins (the platform cannot load them; Settings › Plug-in server uses a computer's over
  Wi-Fi) and no MP3 export yet.
- **Linux**: Windows VST plug-ins run through Wine.
- Plug-in latency and the pitch correction's latency are not compensated yet, so such a track sits slightly late.
- Audio to MIDI is reliable for one note at a time (voice, bass, lead); chord mode is best-effort.
- One tempo and one time signature per project.
- The web edition lacks the newer native features (most built-in effects, wavetables, granular and zones, the slicer,
  the arpeggiator). It opens projects that use them, but may drop those settings when it saves.

## Layout

```
cpp/                 native edition (see cpp/README.md)
  src/engine/        sound engine, project model, plug-in host client
  src/app/           the app's window and views
  src/pluginhost/    undae-pluginhost: AU / VST3 / VST2 host (macOS)
  src/render/        command-line tools: render, compare, self-test, plug-in test
native/              plug-in hosts for Linux (host.c: CLAP, VST3) and Windows (win/vsthost.c: VST2, VST3), PROTOCOL.md
app/                 web edition (index.html, css/, js/audio, js/ui)
desktop/, android/   Electron and Capacitor shells for the web edition
.github/workflows/   the macOS test build and the release build on GitHub Actions
installer/           Windows installer script (Inno Setup) and the release notes
HANDOVER.md          detailed working notes: state, plans, platform specifics
```

## License

JUCE is dual-licensed: AGPLv3, or the JUCE 8 licence (a free "Starter" tier below a revenue limit, paid above). Building
and using it yourself is fine either way; pick one before distributing the native edition.
