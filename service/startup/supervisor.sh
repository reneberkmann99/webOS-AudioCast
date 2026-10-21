#!/bin/sh
# SPDX-License-Identifier: MIT
# Detached supervisor; bounded crash recovery, no routing/mixer/firmware writes.
# Restore interrupted, explicit DILE gain transactions only after their owner exits.
set -u
APP=${AUDIOCAST_APP_DIR:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
STATE=${AUDIOCAST_STATE_DIR:-/var/lib/webos-audiocast}
BIN=$APP/bin/audiocastd
SELF=$APP/service/startup/supervisor.sh
LOCK=$STATE/supervisor.lock
umask 077
TIMEOUT_STYLE=gnu
if command -v timeout >/dev/null 2>&1 && timeout --help 2>&1 | grep -F '[-t SECS]' >/dev/null; then TIMEOUT_STYLE=busybox; fi
timed() {
    seconds=$1; shift
    if [ "$TIMEOUT_STYLE" = busybox ]; then timeout -t "$seconds" "$@"; else timeout "$seconds" "$@"; fi
}
timed_kill() {
    seconds=$1; shift
    if [ "$TIMEOUT_STYLE" = busybox ]; then timeout -t "$seconds" -s KILL "$@"; else timeout -s KILL "$seconds" "$@"; fi
}
recovery_failed=0
recover_dile() {
    [ -f "$STATE/dile-recovery.json" ] || return 0
    [ "$recovery_failed" -eq 0 ] || return 1
    echo 'Interrupted DILE upload-gain transaction: attempting bounded owner-only restoration.'
    if [ -x "$BIN" ] && command -v timeout >/dev/null 2>&1 && timed_kill 8 "$BIN" --state-dir "$STATE" --restore-dile-gain >>"$STATE/daemon.stderr" 2>&1; then
        echo 'DILE upload gain restored; capture will not restart automatically.'
        return 0
    fi
    recovery_failed=1
    : > "$STATE/quarantined"
    echo 'DILE gain recovery failed; capture quarantined. Owner recovery/reboot required.'
    return 1
}
valid_pid() {
    case "$1" in ''|*[!0-9]*) return 1;; esac
    [ "$1" -gt 1 ] && kill -0 "$1" 2>/dev/null && [ -r "/proc/$1/cmdline" ] || return 1
    tr '\000' '\n' < "/proc/$1/cmdline" | grep -F -x -- "$2" >/dev/null 2>&1
}
[ -d "$STATE" ] && [ ! -L "$STATE" ] || exit 1
[ ! -e "$STATE/quarantined" ] || exit 1
if ! mkdir "$LOCK" 2>/dev/null; then
    # Missing owner is never treated as stale: another instance may be creating it.
    [ -r "$LOCK/owner" ] || exit 0
    owner=$(head -n 1 "$LOCK/owner")
    valid_pid "$owner" "$SELF" && exit 0
    rm -f "$LOCK/owner"; rmdir "$LOCK" 2>/dev/null || exit 0
    mkdir "$LOCK" 2>/dev/null || exit 0
fi
printf '%s\n' "$$" > "$LOCK/owner"
printf '%s\n' "$$" > "$STATE/supervisor.pid"
child=''
shutdown_child() {
    if valid_pid "$child" "$BIN"; then
        kill -TERM "$child" 2>/dev/null || true
        count=0
        while valid_pid "$child" "$BIN" && [ "$count" -lt 5 ]; do sleep 1; count=$((count+1)); done
        if valid_pid "$child" "$BIN"; then kill -KILL "$child" 2>/dev/null || true; fi
    fi
    if [ -n "$child" ]; then wait "$child" 2>/dev/null || true; fi
}
cleanup() {
    shutdown_child
    recover_dile || true
    if [ -r "$LOCK/owner" ] && [ "$(head -n 1 "$LOCK/owner")" = "$$" ]; then
        rm -f "$LOCK/owner" "$STATE/supervisor.pid"; rmdir "$LOCK" 2>/dev/null || true
    fi
}
trap 'exit 0' TERM INT HUP
trap cleanup EXIT
failures=0; backoff=1
while [ ! -e "$STATE/stop" ] && [ -x "$BIN" ]; do
    recover_dile || exit 1
    if [ -f "$STATE/supervisor.log" ] && [ "$(wc -c < "$STATE/supervisor.log")" -gt 65536 ]; then : > "$STATE/supervisor.log"; fi
    # Runtime daemon logs are a bounded in-memory ring. stderr only contains fatal process startup errors.
    if [ -f "$STATE/daemon.stderr" ] && [ "$(wc -c < "$STATE/daemon.stderr")" -gt 65536 ]; then
        mv "$STATE/daemon.stderr" "$STATE/daemon.stderr.1"
    fi
    start=$(date +%s)
    "$BIN" --state-dir "$STATE" </dev/null >>"$STATE/daemon.stderr" 2>&1 &
    child=$!
    platform_ticks=0
    while kill -0 "$child" 2>/dev/null && [ -r "/proc/$child/stat" ]; do
        # Observe our own child without an exec-start race; never signal unrelated PIDs.
        if grep -q ') Z ' "/proc/$child/stat"; then break; fi
        if [ -e "$STATE/stop" ] || [ ! -x "$BIN" ]; then exit 0; fi
        if [ "$platform_ticks" -eq 0 ] && command -v timeout >/dev/null 2>&1 && command -v luna-send >/dev/null 2>&1; then
            # Best-effort read-only output telemetry; API absence never changes audio routing.
            timed 2 luna-send -n 1 -w 1800 'luna://com.webos.service.audio/getSoundOut' '{}' 2>/dev/null | head -c 4096 > "$STATE/platform.tmp"
            mv "$STATE/platform.tmp" "$STATE/platform.json"
        fi
        platform_ticks=$(((platform_ticks+1)%5))
        sleep 2
    done
    wait "$child"; code=$?; child=''
    recover_dile || exit 1
    [ -e "$STATE/stop" ] && exit 0
    elapsed=$(($(date +%s)-start))
    if [ "$elapsed" -ge 60 ]; then failures=0;backoff=1; fi
    failures=$((failures+1))
    printf 'AudioCast exited (%s), runtime=%ss, consecutive short failures=%s\n' "$code" "$elapsed" "$failures"
    if [ "$failures" -ge 8 ]; then
        : > "$STATE/quarantined"
        echo 'AudioCast quarantined after repeated failures; open the app / Reconnect to explicitly retry.'
        exit 1
    fi
    count=0
    while [ "$count" -lt "$backoff" ] && [ ! -e "$STATE/stop" ] && [ -x "$BIN" ]; do sleep 1;count=$((count+1));done
    [ "$backoff" -ge 30 ] || backoff=$((backoff*2))
    [ "$backoff" -le 30 ] || backoff=30
done
