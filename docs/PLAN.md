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

### 2. Patches and banks

- Browse by Vital's style tag; favourites.
- Import `.vitaltable` wavetables, `.vitallfo` shapes and samples into
  patches from the UI.
- Tuning files (`.scl`, `.tun`), already supported by the engine.
- Check against a set of real-world patches (Ashley's own, or freely
  licensed community banks). Vital's factory presets can't be bundled.

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

- Spread voices over several cores. Vital processes voices in one thread;
  splitting the voice handler across worker threads is the biggest possible
  gain but also the most invasive engine change.
- Profile the hot paths on the A53 (oscillator, filter, reverb) for
  NEON-specific improvements.
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
  are saved under Vital's own key names (`macro_control_5`, `macro5`...),
  which Vital skips when it opens the patch, so Vital patches load and
  save as before.
- MIDI: rather than MIDI learn on every control, only the macros follow
  MIDI CCs (Ashley, 2026-10-08). Defaults are CC 21 to 28 on any channel:
  undefined in the MIDI spec, clear of the CCs Vital reacts to (mod wheel,
  pedals, MPE slide, bank select), and what many small controllers' knobs
  send. The CC and channel per macro belong to the device, not the patch,
  and are kept in a settings file next to the library.
- Scope: a straight Vital port. Microcontroller synths (Pico, FM-1) can't
  hold Vital's engine and belong in separate projects.

## Done

- Upstream Vital imported under `vital/` with as few changes as possible
  (edits marked `sloppy-synth:`), so upstream fixes stay easy to port.
- Headless engine (`sloppy_engine`, `sloppy::Engine`) built against JUCE's
  non-GUI modules only, with real-time-safe patch loading.
- `.vital` load and save, `.vitalbank` import, patch library by bank and
  folder.
- Linux host `sloppy-synth`: ALSA audio, MIDI with hot-plug and program
  change, and the control server. `sloppy-render` renders patches to WAV
  and doubles as a benchmark.
- Cross builds for aarch64 and armhf, tested under qemu in CI.
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
