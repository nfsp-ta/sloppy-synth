#!/bin/sh
# Installs the APK on a running emulator, starts the app, and checks that
# the synth came up: the process stays alive, and the web UI and its
# control socket answer on the phone's 127.0.0.1.
set -eu

apk="$1"
package=io.github.nfsp_ta.sloppysynth

adb install -r "$apk"
adb logcat -c
adb shell am start -W -n "$package/.MainActivity"

port=""
for _ in $(seq 1 60); do
  port=$(adb logcat -d -s sloppy-synth:I | sed -n 's/.*Web UI on port \([0-9]*\).*/\1/p' | tail -n 1)
  [ -n "$port" ] && break
  sleep 2
done

echo "--- app log"
adb logcat -d -s sloppy-synth:* AndroidRuntime:E DEBUG:* || true

if [ -z "$port" ]; then
  echo "The web UI never started."
  exit 1
fi

sleep 5
if ! adb shell pidof "$package" > /dev/null; then
  echo "The app is no longer running."
  exit 1
fi

adb forward tcp:18080 "tcp:$port"
curl -sSf http://127.0.0.1:18080/ | grep -q "<title>sloppy-synth</title>"
curl -sSf http://127.0.0.1:18080/layout.json > /dev/null
echo "Web UI served on the phone's port $port."

# A WebSocket handshake: the server must switch protocols.
status=$(curl -s -o /dev/null -w '%{http_code}' --max-time 3 \
  -H 'Connection: Upgrade' -H 'Upgrade: websocket' \
  -H 'Sec-WebSocket-Version: 13' -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' \
  http://127.0.0.1:18080/ws || true)
if [ "$status" != "101" ]; then
  echo "WebSocket handshake answered $status, not 101."
  exit 1
fi
echo "Control socket accepts WebSocket connections."

# Rotating the screen must not take the app down: turn to landscape and
# back, then change the font size, which makes Android rebuild the screen
# (the same path some versions take on rotation). Then check the app and its
# server are still there.
check_alive() {
  if ! adb shell pidof "$package" > /dev/null; then
    echo "The app died after $1."
    echo "--- crash log"
    adb logcat -d -b crash || true
    adb logcat -d -s sloppy-synth:* AndroidRuntime:* DEBUG:* libc:* || true
    exit 1
  fi
  curl -sSf http://127.0.0.1:18080/ > /dev/null
  echo "Survived $1."
}

adb shell settings put system accelerometer_rotation 0
for rotation in 1 0 3 0; do
  adb shell settings put system user_rotation "$rotation"
  sleep 3
done
check_alive "rotating the screen"

adb shell settings put system font_scale 1.3
sleep 4
adb shell settings put system font_scale 1.0
sleep 4
check_alive "the screen being rebuilt"
