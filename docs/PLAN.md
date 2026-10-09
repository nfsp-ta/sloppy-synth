# sloppy-synth plan

Goal: Vital's sound engine on cheap hardware. Raspberry Pi 3 on Linux first,
then Android so old phones become synths. A new UI built for small screens:
phones, tablets, and tiny non-touch screens driven by rotary encoders. Vital
patches and banks must keep working.

This file describes what comes next in detail and what is done only briefly.
The history of each change is in the squashed commits on `main`.

## Where we are now

The engine, the Linux host, the web UI and the Android app all have a first
working version. The Android app runs on real phones and tablets; the engine
has not been measured on old phones or a Raspberry Pi yet. While that
hardware is being found, next up is work that needs none: the web UI,
patches and banks, and the Android app.

## Next steps

Roughly in order. Each step is meant to be about one pull request. Steps 1
to 3 need no new hardware; step 4 onwards waits on old phones and a
Pi Zero 2 W, Pi 3 or newer.

### 1. Web UI

- Envelope and LFO shape views.
- Oscilloscope and output level, streamed over the control protocol at a
  low frame rate.
- Bank upload from the browser (the Android app already imports through
  its own Import button).
- Themes: a few built-in ones plus user-made themes. All colours are
  already CSS variables at the top of `web/style.css`, so a theme can be a
  file of those variables that is picked, imported and shared from the UI.
- Control configuration files: export and import one file holding a
  device's control setup (macro CC and channel mappings, the chosen UI
  theme, and later things like per-device limits), to move a setup to
  another device or share it. Not part of patches (Ashley, 2026-10-08).
  The macro mappings already live in a per-device `settings.json`, so the
  export can start as a copy of that file plus the theme.

### 2. Patches and banks

- Save patches from the UI (the engine can save; no UI does yet). When the
  patch uses something Vital can't play, the save warns first. Today that
  is any routing from macros 5 to 8: Vital still opens the file but drops
  those routings (and macros 5 to 8 themselves), so the patch sounds
  different there. The engine already reports this
  (`Engine::getVitalIncompatibilities`, sent as `vital_warnings` with
  `modulations`), and the play page shows it.
- Deferred decision (Ashley, 2026-10-08): what someone can do about such a
  patch. Options so far: list what doesn't fit; an option to strip it
  automatically, for example a "Save for Vital" copy; a standalone patch
  converter; or something else.
- Browse by Vital's style tag; favourites.
- Import `.vitaltable` wavetables, `.vitallfo` shapes and samples into
  patches from the UI.
- Tuning files (`.scl`, `.tun`), already supported by the engine.
- Check against a set of real-world patches (Ashley's own, or freely
  licensed community banks). Vital's factory presets can't be bundled.
- Catch up with Vital 1.6.4. The public source is Vital 1.0.6. Checked
  against Ashley's copy of 1.6.4 through its plugin interface, this engine
  lacks: the Data Compress, Spectral Filter, Spectral Flanger, Spectral
  Phaser and Spectral Contrast warps (plus the per-oscillator warp curve
  and warp phase they use); the Octave + 7 and Sub Harmonics unison
  stacks; ramp up and down on each modulation; a seed for the
  random-amplitude warp; the LFO "Point Cycle" sync; "Multiband" as an
  oscillator destination; the formant filter's third style (an empty stub
  in the public source); and per-patch tunings. Patches using them load
  with warnings. Rebuild them clean-room from how 1.6.4 behaves: settings
  changed through its plugin interface, and renders of the same patch
  through both synths. Never decompile Vital's binaries or commit them:
  they aren't GPL. Background: `research/vital-newer-versions.md` in the
  project files.
- Everything else sounds the same: 73 test patches covering every filter
  model and style, oscillator distortion, spectral warp, unison stack and
  effect render within 0.5 dB of 1.6.4.
- Vital got faster in 1.5.1 and has stayed about the same since (1.6.x is
  5 to 10% slower than 1.5.x). Timed through each release's plugin, 1.5.1
  renders simple patches and effects about 1.5x faster than 1.0.x, two
  16-voice oscillators about 2x, and a spectral warp on a 16-voice unison
  oscillator 3.7x. This engine matches 1.0.x. Building it with clang or
  AVX only gains about 10%, so most of the gain is in Vital's code. Start
  with the spectral warp per unison voice, then profile the rest; see
  step 8. Vital's own `--headless --render` uses
  `SynthBase::renderAudioToFile`, which both engines share, so the same
  comparison can be repeated (it crashes in 1.5.x; use the plugins).

### 3. Android

- Bluetooth MIDI pairing from the app.
- An option to let other devices on the network edit the phone's synth
  (the web UI is served on `127.0.0.1` only today).
- A release signing key, then F-Droid and Google Play (GPLv3 is fine on
  both).

### 4. Measure on real ARM hardware

Nobody knows yet how many voices a phone or a Pi can play. Measured on an
x86 server core (2.1 GHz Xeon, SSE2): the init patch renders 8 voices at
about 20x real time, and a test patch with 4-voice unison, two oscillators
and effects at about 6.5x. A Pi 3's Cortex-A53 at 1.2 GHz is likely around
ten times slower, so light patches should play with a handful of voices and
heavy factory patches may not keep up.

- Android: a benchmark that runs on the device (in the app, or
  `sloppy-render` built with the NDK and run over `adb`), reporting "x real
  time" for a small set of reference patches and voice counts. This part
  can be built early and tried on the Pixel 10a and Galaxy Tab A9+ while
  the old phones are found.
- Raspberry Pi: needs a Pi Zero 2 W, Pi 3 or newer. A Pi 1 can't run the
  engine at all (ARMv6, no NEON). On the board:

  ```sh
  ./sloppy-render patch.vital -n C3,E3,G3,B3 -l 5 -o /tmp/x.wav   # look at "x real time"
  ```

- Write the results down here so later work can be judged against them.

### 5. Per-device limits

Applied when a patch loads, so patches still load, just thinner: cap
polyphony and unison voices, force oversampling to 1x. Defaults per device
class picked from step 4's numbers, adjustable in the UI. Shared by the
Linux and Android hosts.

### 6. Raspberry Pi host

- Real-time setup: `SCHED_FIFO` audio thread, `performance` CPU governor,
  ALSA period sizes of 128 to 256 samples, no desktop running.
- A systemd service so the Pi boots straight into the synth.
- Use 64-bit Raspberry Pi OS. AArch64 NEON has a real vector divide and
  more registers; 32-bit needs a slower reciprocal path.

### 7. Encoder and small-screen UI

A separate client of the control protocol for hardware builds: a 128x64
OLED or 320x240 SPI screen, 4 to 8 rotary encoders with push buttons (read
through `libgpiod`), and a page/menu layout: Oscillators, Filters,
Envelopes, LFOs, Effects, Macros, Patch browser. Each page shows four to
eight parameters mapped to the encoders, taken from `web/layout.json` so it
stays consistent with the web UI. The eight macros get a permanent spot,
since most Vital patches route their key controls through them.

### 8. Performance work

- Spectral warps on unison oscillators: Vital 1.5 and later are about four
  times faster here than this engine (see step 2). Work out a faster way from
  the public 1.0.6 code and measurements alone.
- Spread voices over several cores. Vital processes voices in one thread;
  splitting the voice handler across worker threads is the biggest possible
  gain but also the most invasive engine change.
- Profile the hot paths on the A53 (oscillator, filter, reverb) for
  NEON-specific improvements. On x86 the engine now renders as fast as
  Vital 1.6.4 in the same render path, and Vital 1.5.x is 5 to 10% faster
  than that. What's left in the profile is spread thin: oscillators,
  modulation sums, envelopes, the reverb and delay, and processor routing,
  each a few percent.
- Longer term, shrink the engine's JUCE use to what it really needs (zip,
  WAV, FFT, strings, files) to make Android and embedded builds lighter.

A Pi 4 or 5 is two to five times faster than a Pi 3 and is a safe fallback
target if the Pi 3 can't keep up.

## Architecture

```
               +-------------------------------+
               |  sloppy_engine (C++, no UI)   |
               |  Vital DSP + patch/bank layer |
               +---------------+---------------+
                               |  C++ API (sloppy::Engine)
          +--------------------+---------------------+
          |                                          |
 +--------+---------+                      +---------+---------+
 | Linux host       |                      | Android host      |
 | sloppy-synth     |                      | NDK lib + Oboe    |
 | ALSA + MIDI      |                      | + Android MIDI    |
 +--------+---------+                      +---------+---------+
          |              control protocol            |
          |   (parameters, patches, meters, scope)   |
          +--------------------+---------------------+
                               |
       +-----------------------+------------------------+
       |                       |                        |
  Web UI (phone,         Encoder + small         (later) desktop,
  tablet, browser)       screen UI on the Pi     MIDI controller maps
```

Every UI is a client of one control protocol (JSON over a WebSocket, see
`docs/PROTOCOL.md`) rather than being linked into the engine. One UI
codebase serves a phone, a tablet and a Pi, and a Pi can be played and
edited from a phone over Wi-Fi. Parameter names are Vital's own
(`filter_1_cutoff` etc.), which are also the keys in `.vital` files.

Decisions so far:

- UI: a web UI served by the synth itself, also shown in the Android app's
  WebView (Ashley, 2026-10-07). Native per-platform UIs and a reworked
  JUCE UI were turned down: two UIs to keep in sync, or OpenGL and a large
  desktop window that a Pi 3 can't drive.
- Look: a neutral, slightly blue-gray dark theme, not Vital's purple
  (Ashley, 2026-10-07).
- Android: a small Kotlin app with the engine as an NDK library, rather
  than a full JUCE Android app.
- Macros: eight instead of Vital's four (Ashley, 2026-10-08). Macros 5 to 8
  are saved under Vital's own key names (`macro_control_5`, `macro5`...).
  Vital patches load and save as before. Vital can open sloppy-synth's
  patches too (checked with an unmodified 4-macro build), but skips
  macros 5 to 8 and their routings.
- MIDI: rather than MIDI learn on every control, only the macros follow
  MIDI CCs (Ashley, 2026-10-08). Defaults are CC 21 to 28 on any channel:
  undefined in the MIDI spec, clear of the CCs Vital reacts to (mod wheel,
  pedals, MPE slide, bank select), and what many small controllers' knobs
  send. The CC and channel per macro belong to the device, not the patch,
  and are kept in a per-device settings file (`~/.config/sloppy-synth` on
  Linux, next to the library on Android).
- Scope: a straight Vital port. Microcontroller synths (Pico, FM-1) can't
  hold Vital's engine and belong in separate projects.

## Done

- Upstream Vital imported under `vital/` with as few changes as possible
  (edits marked `sloppy-synth:`), so upstream fixes stay easy to port.
- Headless engine (`sloppy_engine`, `sloppy::Engine`) built against JUCE's
  non-GUI modules only, with real-time-safe patch loading.
- `.vital` load and save, `.vitalbank` import, patch library by bank and
  folder. Patches from newer 1.x releases load too, with warnings for
  anything this engine lacks.
- Linux host `sloppy-synth`: ALSA audio, MIDI with hot-plug and program
  change, and the control server. `sloppy-render` renders patches to WAV
  and doubles as a benchmark.
- Cross builds for aarch64 and armhf, tested under qemu in CI.
- Vital's FFT runs on pffft (`third_party/pffft`) in place of JUCE's
  portable FFT. Spectral warps on unison voices became 3x faster, and other
  test patches take 18 to 31% less time.
- Web UI: play page with macros and a multi-touch keyboard, patch browser,
  parameter pages from `web/layout.json`, and modulation routing.
- Eight macros, each on a MIDI CC and channel set from the web UI (with
  MIDI learn), saved per device.
- Android app: engine over NDK, Oboe audio, USB and virtual MIDI, web UI in
  a WebView, background service, patch and bank import. CI builds the APK
  and smoke-tests it on two emulator API levels.

## Rules from upstream

From Vital's README, binding on this fork: GPLv3; no "Vital", "Vital Audio",
"Tytel" or "Matt Tytel" in product names or marketing; no connections to
vital.audio services; no bundling Vital's free factory presets; no iOS App
Store builds.
