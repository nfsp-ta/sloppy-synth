# sloppy-synth plan

Goal: Vital's sound engine on cheap hardware. Raspberry Pi 3 on Linux first,
then Android so old phones become synths. A new UI built for small screens:
phones, tablets, and tiny non-touch screens driven by rotary encoders. Vital
patches and banks must keep working.

## 1. Where things stand

Done in the first PR:

- Upstream Vital imported under `vital/` with as few changes as possible, so
  upstream fixes stay easy to compare and port.
- `sloppy_engine`: Vital's synthesis engine and patch layer built against
  JUCE's non-GUI modules only. No X11, OpenGL, curl or display.
- `sloppy::Engine`: one object for hosts to drive (prepare, process a block
  with MIDI, load or save a patch, get or set parameters by Vital's names).
  Real-time safe in the sense that a patch load on another thread never
  blocks the audio callback; the callback outputs silence for that block.
- `sloppy::PatchLibrary`: `.vitalbank` import (unzips the way Vital does,
  rejects paths that escape the library), patch listing by bank and folder.
- `sloppy-synth`: headless ALSA player with MIDI input, a virtual MIDI port,
  program-change patch switching and hot-plugged controllers.
- `sloppy-render`: patch to WAV, doubling as a speed benchmark.
- Cross builds for aarch64 and armhf, tested under qemu. ARM output matches
  x86 to within 0.2% (float rounding between NEON and SSE).

## 2. Architecture

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
 | ALSA/JACK + MIDI |                      | + Android MIDI    |
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

The key decision: every UI is a client of one control protocol rather than
being linked into the engine. Then one UI codebase can serve a phone, a
tablet and a Pi, and the Pi can be played and edited from a phone over Wi-Fi.

Control protocol (proposal): JSON messages over a WebSocket, served by the
host process, for parameter get/set/subscribe, patch browse/load/save, bank
import, and a few streamed visuals (output level, oscilloscope, LFO and
envelope positions) at a low frame rate. Parameter names are Vital's own
(`filter_1_cutoff` etc.), which are also the keys in `.vital` files.

## 3. Raspberry Pi 3: performance is the main risk

Vital is heavy. The engine is single-threaded and the Pi 3's Cortex-A53 at
1.2 GHz is far slower than a desktop core. Measured on an x86 server core
(2.1 GHz Xeon, SSE2): the init patch renders 8 voices at about 20x real
time, and a test patch with 4-voice unison, two oscillators and effects at
about 6.5x. A Pi 3 core will likely be around ten times slower, so light
patches should play with a handful of voices and heavy factory patches may
not keep up. This needs measuring on the real board first:

```sh
./sloppy-render patch.vital -n C3,E3,G3,B3 -l 5 -o /tmp/x.wav   # look at "x real time"
```

Levers, roughly in order of payoff:

1. Use 64-bit Raspberry Pi OS. AArch64 NEON has a real vector divide and
   more registers; 32-bit needs a slower reciprocal path.
2. Per-device limits applied at load time: cap polyphony and unison voices,
   force oversampling to 1x. Patches still load, just thinner.
3. Real-time setup: `SCHED_FIFO` audio thread, `performance` CPU governor,
   ALSA period sizes of 128 to 256 samples, no desktop running.
4. Spread voices over the Pi's four cores. Vital processes voices in one
   thread; splitting the voice handler across worker threads is the biggest
   possible gain but also the most invasive engine change.
5. Profile the hot paths on the A53 (oscillator, filter, reverb) for
   NEON-specific improvements.

A Pi 4 or 5 is two to five times faster than a Pi 3 and would be a safe
fallback target.

## 4. Android

Recommended approach: a small Kotlin app with the engine as an NDK library.

- Audio via Oboe (low-latency AAudio on Android 8.1+, OpenSL ES below),
  calling `Engine::process` from the Oboe callback. JUCE's audio device
  layer isn't needed on Android.
- MIDI via `android.media.midi` (USB and Bluetooth MIDI, Android 6+).
- UI: a WebView showing the same web UI, talking to the engine through a
  local WebSocket or a JS bridge.
- ABIs: `arm64-v8a` and `armeabi-v7a`, `minSdk` 23 (Android 6) to reach old
  phones while still having the MIDI API.
- The CMake build already handles `ANDROID` and `armeabi-v7a`; the work is
  the Gradle project, JNI glue, and checking that the JUCE core pieces the
  engine uses (files, strings, threads) behave without a JUCE app shell.

Alternative: a full JUCE Android app. Less glue code, but it pulls JUCE's
Android app framework in and makes a web-based UI awkward. Not recommended
unless we pick a JUCE UI.

Longer term, shrinking the engine's JUCE use to what it really needs (zip,
WAV, FFT, strings, files) would make Android and embedded builds lighter.

Distribution: GPLv3 apps are fine on Google Play and F-Droid (unlike the iOS
App Store, which upstream rules out).

## 5. UI options (decision needed)

All of these talk to the engine through the control protocol above.

**A. Web UI (recommended).** HTML/JS served by the synth itself. One
codebase covers phone portrait, tablet landscape and desktop with a
responsive layout; works in Android's WebView; lets a phone edit a Pi over
the network. Pi 3 never has to draw it. Cost: an HTTP/WebSocket server in
the host, and a frontend stack to pick (a small framework such as Svelte or
Preact, built ahead of time so the synth just serves static files).

**B. Native UIs per platform.** Jetpack Compose on Android, something else
on Linux. Best native feel on Android, but two UIs to build and keep in sync,
and no phone-edits-the-Pi for free.

**C. Rework Vital's JUCE UI for small screens.** Reuses the most existing
code, but Vital's UI depends on OpenGL and is designed around a large
desktop window; a Pi 3 can't drive it well and it doesn't map to encoders.
Not recommended.

**Encoder and tiny-screen UI (any of the above).** A separate client for
hardware builds: a 128x64 OLED or 320x240 SPI screen, 4 to 8 rotary encoders
with push buttons (read through `libgpiod`), and a page/menu layout:
Oscillators, Filters, Envelopes, LFOs, Effects, Macros, Patch browser. Each
page shows four to eight parameters mapped to the encoders. The page layout
lives in one JSON file that the web UI's compact "performance" view reuses,
so both stay consistent. Macros 1 to 4 deserve a permanent spot, since most
Vital patches route their key controls through them.

## 6. Patches and banks

Done: `.vital` load and save (including Vital's upgrade path for older
patches; patches from a newer major/minor Vital are refused just like Vital
does), `.vitalbank` import, library listing.

Next:

- Browse by bank, folder and Vital's style tag; favourites.
- Import `.vitaltable` wavetables, `.vitallfo` shapes and samples into
  patches from the UI.
- Tuning files (`.scl`, `.tun`), already supported by the engine.
- Check against a set of real-world patches (Ashley's own, or freely
  licensed community banks). Vital's factory presets can't be bundled.

## 7. Milestones

1. Headless engine on ARM Linux, patch and bank import. *(this PR)*
2. Measure on a Pi 3; per-device voice and unison limits; real-time tuning;
   a systemd service so the Pi boots straight into the synth.
3. Control protocol and server in `sloppy-synth`.
4. First web UI: patch browser, macros, a performance page, then the full
   parameter pages.
5. Encoder/small-screen UI on the Pi.
6. Android app: NDK engine, Oboe, MIDI, WebView UI.
7. Performance work: multi-core voices, NEON tuning.

## 8. Rules from upstream

From Vital's README, binding on this fork: GPLv3; no "Vital", "Vital Audio",
"Tytel" or "Matt Tytel" in product names or marketing; no connections to
vital.audio services; no bundling Vital's free factory presets; no iOS App
Store builds.
