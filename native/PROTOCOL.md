# Plug-in host protocol (version 1)

The plug-in host is a separate process. It listens for one WebSocket connection per plug-in instance on
`ws://127.0.0.1:<port>/?t=<token>` and refuses connections without the token or from a non-local `Origin`.
On start it prints one line of JSON to stdout: `{"port":N,"token":"..."}`.

The app side is `app/js/audio/bridge.js`. `native/ref-host.js` is a stand-in that implements this protocol without
loading plug-ins (it halves the level), used to test the bridge.

## Audio: binary messages, little-endian

| offset | type        | meaning                              |
|--------|-------------|--------------------------------------|
| 0      | u32         | message type, `1` = audio block      |
| 4      | u32         | audio frame the block starts at (the app's clock, low 32 bits) |
| 8      | u32         | frame count `n` (128 by default)     |
| 12     | f32 × 2n    | stereo samples, interleaved L R      |

The host answers every audio block with one block of the same shape and frame count, in order. The app buffers
`latency` frames (1024 to start, 1536 for the Windows host) before it starts playing the returned audio; that buffer is
the delay the bridge adds, and it grows by 512 frames after each dropout, up to 4096.

## Control: text messages, JSON

Requests carry `op` and a `seq` number; replies carry `re` (the op), the same `seq`, `ok`, and `error` when `ok` is false.

| op       | request fields                         | reply fields                                                        |
|----------|----------------------------------------|---------------------------------------------------------------------|
| `hello`  |                                        | `host`, `version`, `formats`, `gui`                                 |
| `scan`   | `paths` (extra folders; `~/.clap`, `/usr/lib/clap`, `/usr/local/lib/clap` and `$CLAP_PATH` are always scanned) | `plugins`: `[{ id, name, vendor, version, path, format, instrument }]` |
| `load`   | `path`, `id` (empty = first in the file), `sampleRate` | `id`, `name`, `vendor`, `latency`, `inputs`, `outputs` (channels of the main ports), `notes`, `state`, `gui`, `params`: `[{ id, name, module, min, max, def, value, text, stepped, hidden, readonly }]` |
| `params` |                                        | `params`: `[{ id, value, text }]`                                   |
| `state`  | `data` (base64) to restore, or none to read | `data` when reading; `params` after a restore                  |
| `gui`    | `show` (bool)                          | `open`                                                              |
| `unload` |                                        |                                                                     |
| `param`  | `id`, `value`                          | no reply; applied at the start of the next audio block              |
| `note`   | `on`, `key`, `vel` (0..1), `at` (optional audio frame, same clock as the block header) | no reply; applied at that frame, or at the start of the next block when `at` is absent or already past |
| `midi`   | `data` ([status, d1, d2]), `at` (optional frame) | no reply; pitch bend, controllers (see HANDOVER.md for how each format takes them) |

Events from the host (no `seq`):

- `{"ev":"params","params":[...]}`: the plug-in says its parameter list or values changed.
- `{"ev":"param","changes":[{ id, value, text }]}`: the user changed parameters in the plug-in's own window.
- `{"ev":"gui","open":false}`: the user closed the plug-in's window.
- `{"ev":"dirty"}`: the plug-in's saved state changed; read it with `state`.

With no plug-in loaded, audio blocks are returned unchanged. `native/ref-host.js` implements only `hello`, `scan` (empty)
and audio.

## The Windows host

`native/win/vsthost.c` (run under Wine) speaks this same protocol for Windows VST2 plug-ins. Differences: `hello` reports
`formats: ["vst2", "vst3"]` and `bits`; `scan` looks only in the folders given in `paths`; parameter ids are the plug-in's parameter indexes
and every parameter runs 0 to 1; `gui` opens the plug-in's editor in a Wine window; `dirty` is sent after a change in that window and when it closes; `state`
data starts with `C` (the plug-in's own chunk) or `P` (its parameter values).

`panic` (no reply): drop queued notes and parameter changes and reset the plug-in, so it falls silent at once. The app
sends it to plug-in instruments when the transport stops.
