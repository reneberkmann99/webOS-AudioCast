#!/bin/sh
# SPDX-License-Identifier: MIT
# Local, fixed-action Homebrew helper. Never accepts shell commands or network input.
set -eu
APP=${AUDIOCAST_APP_DIR:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
STATE=${AUDIOCAST_STATE_DIR:-/var/lib/webos-audiocast}
HOOKS=${AUDIOCAST_HOOK_DIR:-/var/lib/webosbrew/init.d}
BIN=$APP/bin/audiocastd
SUP=$APP/service/startup/supervisor.sh
HOOK=$HOOKS/audiocast
[ "$(id -u)" = 0 ] || { echo 'AudioCast requires the Homebrew root service, not Developer Mode alone.' >&2; exit 1; }
[ ! -L "$STATE" ] || { echo 'Refusing symlink state directory' >&2; exit 1; }
mkdir -p "$STATE"; chmod 700 "$STATE"; umask 077
valid_pid() {
    case "$1" in ''|*[!0-9]*) return 1;; esac
    [ "$1" -gt 1 ] && kill -0 "$1" 2>/dev/null || return 1
    [ -r "/proc/$1/cmdline" ] || return 1
    tr '\000' '\n' < "/proc/$1/cmdline" | grep -F -x -- "$2" >/dev/null 2>&1
}
read_pid() { if [ -r "$1" ]; then head -n 1 "$1"; fi; }
action=${1:-status}
case "$action" in
    ensure)
        [ -x "$BIN" ] && [ -r "$SUP" ] || { echo 'AudioCast native binary/helpers missing; reinstall the complete IPK.' >&2; exit 1; }
        # Bootstrap initializes mode0600 credentials/config and prints only a JSON credential reply.
        reply=$("$BIN" --bootstrap --state-dir "$STATE")
        rm -f "$STATE/stop" "$STATE/quarantined"
        pid=$(read_pid "$STATE/supervisor.pid")
        if ! valid_pid "$pid" "$SUP"; then
            command -v nohup >/dev/null 2>&1 || { echo 'nohup is required for detached Homebrew persistence' >&2; exit 1; }
            if command -v setsid >/dev/null 2>&1; then
                AUDIOCAST_APP_DIR="$APP" AUDIOCAST_STATE_DIR="$STATE" nohup setsid sh "$SUP" </dev/null >>"$STATE/supervisor.log" 2>&1 &
            else
                AUDIOCAST_APP_DIR="$APP" AUDIOCAST_STATE_DIR="$STATE" nohup sh "$SUP" </dev/null >>"$STATE/supervisor.log" 2>&1 &
            fi
        fi
        printf '%s\n' "$reply"
        ;;
    enable)
        [ -x "$APP/service/startup/boot-hook.sh" ] || { echo 'Boot hook missing' >&2; exit 1; }
        mkdir -p "$HOOKS"
        if [ -e "$HOOK" ] || [ -L "$HOOK" ]; then
            [ -L "$HOOK" ] && [ "$(readlink "$HOOK")" = "$APP/service/startup/boot-hook.sh" ] || { echo 'Refusing to replace foreign startup file' >&2; exit 1; }
        else ln -s "$APP/service/startup/boot-hook.sh" "$HOOK"; fi
        echo 'AudioCast startup symlink enabled (service starts at boot)'
        ;;
    disable)
        if [ -L "$HOOK" ]; then
            [ "$(readlink "$HOOK")" = "$APP/service/startup/boot-hook.sh" ] || { echo 'Refusing to remove foreign startup link' >&2; exit 1; }
            rm -f "$HOOK"
        elif [ -e "$HOOK" ]; then echo 'Refusing to remove foreign startup file' >&2; exit 1; fi
        echo 'AudioCast startup symlink disabled'
        ;;
    stop)
        : > "$STATE/stop"
        for entry in "$STATE/supervisor.pid:$SUP" "$STATE/daemon.pid:$BIN"; do
            file=${entry%%:*}; expected=${entry#*:}; pid=$(read_pid "$file")
            if valid_pid "$pid" "$expected"; then kill -TERM "$pid"; fi
        done
        echo 'AudioCast shutdown requested; resources release after current bounded operation'
        ;;
    status)
        pid=$(read_pid "$STATE/daemon.pid")
        if valid_pid "$pid" "$BIN"; then echo 'AudioCast daemon process is running (use authenticated status API for capture health)'; else echo 'AudioCast daemon is not running'; fi
        ;;
    diagnose)
        [ -r "$APP/tools/diagnose.sh" ] || { echo 'Diagnostic tool missing' >&2; exit 1; }
        AUDIOCAST_BIN="$BIN" sh "$APP/tools/diagnose.sh"
        ;;
    *) echo 'Allowed actions: ensure enable disable stop status diagnose' >&2; exit 2;;
esac
