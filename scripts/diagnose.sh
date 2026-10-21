#!/bin/sh
# SPDX-License-Identifier: MIT
# Non-destructive inventory. No routing/mixer writes, no audio saved, no microphone fallback.
DEVICE=''
case ${1:-} in
    '') ;;
    --device) DEVICE=${2:-}; printf '%s\n' "$DEVICE" | grep -E '^hw:[0-9]{1,3},[0-9]{1,3}$' >/dev/null || { echo 'Use --device hw:CARD,DEVICE' >&2; exit 2; };;
    *) echo 'Usage: diagnose.sh [--device hw:CARD,DEVICE]' >&2; exit 2;;
esac
section() { printf '\n### %s\n' "$1"; }
run() {
    printf '\n$'; for arg do printf ' %s' "$arg"; done; printf '\n'
    if ! command -v "$1" >/dev/null 2>&1; then printf 'UNAVAILABLE: %s\n' "$1"; return; fi
    if command -v timeout >/dev/null 2>&1; then timeout 5 "$@" 2>&1; rc=$?; else
        case "$1" in uname|cat|id|ls) "$@" 2>&1;rc=$?;;
        *) echo 'SKIPPED: timeout utility absent; refusing potentially blocking vendor/service command';return;; esac
    fi
    [ "$rc" = 0 ] || printf 'Command exit status: %s (124 usually means timeout)\n' "$rc"
}
file() { if [ -r "$1" ]; then run cat "$1"; else printf 'NOT PRESENT/READABLE: %s\n' "$1"; fi; }
echo 'webOS AudioCast diagnostic inventory 0.1.0'
echo 'Contains TV/system/network identifiers; review before sharing. No credentials/environment/audio are dumped.'
section 'Kernel, architecture and identity';run uname -a;run uname -m;run id
for f in /etc/webos-release /etc/os-release /etc/lgmodel /proc/device-tree/model /sys/devices/soc0/family /sys/devices/soc0/soc_id /sys/devices/soc0/machine;do file "$f";done
section 'CPU and ALSA inventory';file /proc/cpuinfo;file /proc/asound/cards;file /proc/asound/pcm;file /proc/asound/version
run arecord -l;run arecord -L
section 'Mixer READ ONLY — values are never changed';run amixer scontents
section 'Audio output and TV model — read-only private APIs, firmware dependent'
run luna-send -n 1 'luna://com.webos.service.audio/getSoundOutput' '{}'
run luna-send -n 1 'luna://com.webos.service.tv.systemproperty/getSystemInfo' '{"keys":["modelName","firmwareVersion","sdkVersion"]}'
section 'Homebrew root service health — fixed id command only'
run luna-send -n 1 'luna://org.webosbrew.hbchannel.service/exec' '{"command":"id -u"}'
section 'Utilities and relevant libraries'
for utility in arecord amixer aplay pactl parec pulseaudio luna-send luna-send-pub node nohup timeout readlink grep; do
    if command -v "$utility" >/dev/null 2>&1;then command -v "$utility";else echo "$utility: unavailable";fi
done
# List filenames only; do not dump proprietary binaries or configuration secrets.
for pattern in /usr/lib/libasound.so* /lib/libasound.so* /usr/lib/libpulse.so* /usr/lib/libdile*audio* /usr/lib/liblgnc*;do [ ! -e "$pattern" ] || ls -l "$pattern";done
section 'PulseAudio monitor inventory, only if an actual server responds'
run pactl info;run pactl list short sources
section 'Currently open PCM hardware params (not full capabilities)'
for f in /proc/asound/card*/pcm*c/sub*/hw_params;do [ ! -r "$f" ] || file "$f";done
section 'Explicit capture capability query'
if [ -n "$DEVICE" ]; then
    BIN=${AUDIOCAST_BIN:-/media/developer/apps/usr/palm/applications/org.webosbrew.audiocast/bin/audiocastd}
    echo "Explicitly querying $DEVICE; even opening vendor capture hardware may have firmware-specific side effects."
    if [ -x "$BIN" ];then run "$BIN" --probe-device "$DEVICE";else
        echo 'Native query binary unavailable. Run arecord --dump-hw-params only as a separate explicit capture test.'
        echo 'No automated recording or hardware format application is performed by this inventory.'
    fi
else echo 'Not requested. Re-run with --device hw:CARD,DEVICE to query supported native constraints without applying capture params.';fi
section 'Interpretation'
echo 'Successful open, capabilities, or nonzero levels DO NOT prove TV playback capture.'
echo 'Use known authorized TV playback; correlate start/stop with levels, compare audible capture on receiver, and verify speakers remain normal.'
echo 'If no supported path exists, report unsupported. Never route output, set mixer controls, use microphone input, or bypass protected media restrictions.'
