# sloppy-synth

A fork of the [Vital](https://github.com/mtytel/vital) spectral warping
wavetable synthesizer, reworked to run headless on small ARM Linux boards
(starting with the Raspberry Pi 3) and on Android, so old phones and cheap
boards can become synths. It plays Vital patches (`.vital`) and imports
Vital banks (`.vitalbank`).

The desktop UI is being replaced; see [docs/PLAN.md](docs/PLAN.md) for the
plan for Android and for small-screen, rotary-encoder and web UIs.

## What's here

| Path | What it is |
| --- | --- |
| `vital/` | Upstream Vital source (mtytel/vital `636ca0e`), kept close to upstream. Patches to it are marked `sloppy-synth:` |
| `engine/` | The headless engine: `sloppy::Engine` (play, load patches, parameters) and `sloppy::PatchLibrary` (banks). JUCE config with no GUI modules |
| `apps/sloppy-synth` | Real-time player: loads a patch, plays it from MIDI through ALSA |
| `web/` | The web UI for phones, tablets and desktops, served by `sloppy-synth` |
| `android/` | The Android app: the engine as an NDK library, Oboe for audio, Android MIDI, and the web UI in a WebView |
| `apps/sloppy-render` | Offline renderer: patch + notes in, WAV out. Also a quick benchmark |
| `tests/` | Engine tests and generated test patches |
| `cmake/toolchains/` | Cross-compile setups for 64-bit and 32-bit Raspberry Pi OS |

## Building

Needs CMake 3.16+, a C++14 compiler and the ALSA headers.

```sh
sudo apt install build-essential cmake ninja-build libasound2-dev
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build
```

This also works directly on a Raspberry Pi, but on a Pi 3 it is slow and
tight on memory: the two big engine files each need about 800 MB to
compile, so build with `cmake --build build -j1` and some swap. Cross-
compiling (below) or taking the binaries CI builds is much quicker.

`-DSLOPPY_WITH_AUDIO_DEVICES=OFF` builds only the engine and `sloppy-render`,
with no ALSA dependency.

### Cross-compiling for a Raspberry Pi

On Debian or Ubuntu:

```sh
sudo dpkg --add-architecture arm64      # or armhf for 32-bit Raspberry Pi OS
sudo apt update
sudo apt install crossbuild-essential-arm64 libasound2-dev:arm64 qemu-user
cmake -B build-arm64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-gnu.cmake
cmake --build build-arm64
ctest --test-dir build-arm64        # runs the ARM binaries through qemu
```

Use `arm-linux-gnueabihf.cmake` with `armhf` packages for 32-bit Raspberry
Pi OS. Build on a distribution with the same or an older glibc than the Pi
(Debian bookworm matches current Raspberry Pi OS); CI does this and uploads
ready-to-run binaries for x86_64, aarch64 and armhf.

## Running

```sh
# Play a patch from any connected MIDI controller
./sloppy-synth "My Patch.vital"

# Check audio without a controller: play C3 for a second
./sloppy-synth "My Patch.vital" --audition C3

# Import a bank, list the library, start on patch 5
./sloppy-synth --import-bank "Some Bank.vitalbank" --list-patches
./sloppy-synth --patch-index 5

# Render to a WAV, and see how much faster than real time the engine runs
./sloppy-render "My Patch.vital" -n C3,E3,G3,B3 -l 4 -o out.wav
```

`sloppy-synth` also serves the web UI, on port 8080 by default (or the next
free port if another program has 8080), and prints its address at startup. Open
it on a phone or tablet on the same network to
play, browse patches and edit sounds; several devices can be connected at
once and stay in sync. `--http-bind 127.0.0.1` keeps it local and `--no-web`
turns it off. The protocol is in [docs/PROTOCOL.md](docs/PROTOCOL.md).

On a computer, the keyboard plays the piano in the web UI: the A row is the
white keys from C, the row above it the black keys, and Z and X change octave.
The letters are shown on the piano keys.

MIDI program changes switch between library patches (bank select MSB picks
the next group of 128). The library lives in `~/.local/share/sloppy-synth`
unless `--library` or `$SLOPPY_DATA_DIR` says otherwise; it can also point
at an existing Vital data folder.

On a Pi, `sloppy-render` reporting well above `1x real time` for the number
of notes you want to play is a good sign the patch will run live.

## Android

The app in `android/` runs the same engine and serves the same web UI as
the Linux player, shown full screen in a WebView. It needs Android 6 or
newer on a 64-bit or 32-bit ARM phone with NEON (nearly all of them), or
x86_64 for the emulator.

- **Audio:** Oboe, which uses AAudio on Android 8.1+ and OpenSL ES before
  that. The buffer starts small and grows by itself whenever the phone can't
  keep up, so old phones trade latency for no dropouts.
- **MIDI:** USB MIDI keyboards and other apps' MIDI ports are opened
  automatically, including ones plugged in later. Program changes switch
  patches, as on the Linux player.
- **Background:** the synth keeps playing with the screen off or the app in
  the background, so a phone can sit on a desk as a sound module. Stop it
  from its notification. Back leaves it playing.
- **Patches:** the Import button in the patch browser takes a `.vital` patch
  or `.vitalbank` bank. Opening or sharing one from a file manager or
  browser works too. The library is in
  `Android/data/io.github.nfsp_ta.sloppysynth/files/library`, where files can
  also be copied over USB.
- **Web UI:** served on the phone's `127.0.0.1` only (port 8080, or the next
  free one).

CI builds an installable APK (`sloppy-synth-android` under the run's
artifacts) and starts it on an emulator. To build it yourself, install
Android Studio or the Android SDK with NDK 27, then:

```sh
cd android
./gradlew assembleRelease
adb install app/build/outputs/apk/release/app-release.apk
```

Release builds are signed with the debug key for now, so they install for
testing but can't go to an app store yet.

## License and naming

sloppy-synth is licensed under the GNU GPL v3, like Vital; see
[LICENSE](LICENSE). Vital is by Matt Tytel.

Per the upstream terms: builds of this fork must not be called "Vital",
must not connect to vital.audio services, must not bundle Vital's factory
presets, and may not be distributed on the iOS App Store.
