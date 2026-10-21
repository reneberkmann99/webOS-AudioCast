#!/bin/sh
# SPDX-License-Identifier: MIT
# Installed only as a symlink in Homebrew init.d. Return promptly; never block TV boot.
APP=/media/developer/apps/usr/palm/applications/org.webosbrew.audiocast
STATE=/var/lib/webos-audiocast
[ -x "$APP/bin/audiocastd" ] && [ -r "$STATE/config.json" ] || exit 0
[ ! -e "$STATE/quarantined" ] || exit 0
grep -q '"autostart": true' "$STATE/config.json" || exit 0
grep -q '"playback_confirmed": true' "$STATE/config.json" || exit 0
[ -e "$STATE/stop" ] && rm -f "$STATE/stop"
# supervisor owns duplicate prevention and crash backoff; native daemon owns capture recovery.
if command -v setsid >/dev/null 2>&1; then
    AUDIOCAST_APP_DIR="$APP" AUDIOCAST_STATE_DIR="$STATE" nohup setsid sh "$APP/service/startup/supervisor.sh" </dev/null >>"$STATE/supervisor.log" 2>&1 &
else
    AUDIOCAST_APP_DIR="$APP" AUDIOCAST_STATE_DIR="$STATE" nohup sh "$APP/service/startup/supervisor.sh" </dev/null >>"$STATE/supervisor.log" 2>&1 &
fi
exit 0
