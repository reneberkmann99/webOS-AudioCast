#!/usr/bin/env bash
# TV audio -> Mac: pull the TV's AudioCast HTTP stream and play it into a chosen
# CoreAudio OUTPUT device.
#
#   * To expose the TV as an INPUT device (for any app / routing): target a BlackHole
#     loopback device. Audio played into BlackHole's output appears on its input side.
#   * To just hear the TV: target AirPods (or the system default output).
#   * To do both: create a Multi-Output Device = [BlackHole + AirPods] in
#     Audio MIDI Setup and target that.
#
# Usage:
#   tv-audio-mac.sh list-devices                       # enumerate output devices
#   tv-audio-mac.sh <device-name-or-index> [tv-host]   # pull + play into device
#
# First run fetches the stream token from the TV (one-time) if it's missing.
set -euo pipefail

TV_HOST="${TV_HOST:-10.10.40.104}"
TV_PORT="${TV_PORT:-7070}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RECEIVER="${RECEIVER:-$ROOT/receiver/receiver.py}"
TOKEN_FILE="${TOKEN_FILE:-$HOME/.config/audiocast/stream-token}"

list_devices() {
  python3 - <<'PY'
import sys
try:
    import sounddevice as sd
except ImportError:
    print("sounddevice not installed.", file=sys.stderr)
    print("Install:  brew install portaudio && pip3 install sounddevice", file=sys.stderr)
    sys.exit(1)
for i, d in enumerate(sd.query_devices()):
    if d.get('max_output_channels', 0) > 0:
        print(f"{i}: {d['name']}  ({d['max_output_channels']} ch)")
PY
}

fetch_token() {
  local host="$1"
  echo "Fetching stream token from TV $host ..." >&2
  ssh root@"$host" 'cat /var/lib/webos-audiocast/credentials.json' \
    | python3 -c 'import sys,json; print(json.load(sys.stdin)["stream_token"])' \
    > "$TOKEN_FILE"
  chmod 600 "$TOKEN_FILE"
  echo "Stored stream token in $TOKEN_FILE" >&2
}

case "${1:-}" in
  list-devices|devices)
    list_devices
    exit 0
    ;;
  fetch-token)
    fetch_token "${2:-$TV_HOST}"
    exit 0
    ;;
  ""|-h|--help)
    echo "usage: $0 {list-devices|fetch-token [host]|<device-name-or-index> [tv-host]}" >&2
    exit 2
    ;;
esac

DEVICE="$1"
TV_HOST="${2:-$TV_HOST}"

if [ ! -r "$TOKEN_FILE" ]; then
  fetch_token "$TV_HOST"
fi

echo "Pulling TV audio from http://$TV_HOST:$TV_PORT/audio -> device \"$DEVICE\" (Ctrl-C to stop)" >&2
exec python3 "$RECEIVER" http \
  --url "http://$TV_HOST:$TV_PORT/audio" \
  --token-file "$TOKEN_FILE" \
  --play --device "$DEVICE"
