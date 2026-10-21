#!/bin/sh
# SPDX-License-Identifier: MIT
# Installed only as a symlink in Homebrew init.d. Return promptly; never block TV boot.
# Starts the BACKGROUND SERVICE (daemon) at boot so the GUI can connect immediately.
# Whether streaming itself auto-starts is governed by the daemon's own config.autostart.
APP=${AUDIOCAST_APP_DIR:-/media/developer/apps/usr/palm/applications/org.webosbrew.audiocast}
STATE=${AUDIOCAST_STATE_DIR:-/var/lib/webos-audiocast}
[ -x "$APP/bin/audiocastd" ] || exit 0
[ ! -L "$STATE" ] || exit 1
umask 077
mkdir -p "$STATE" || exit 1
chmod 700 "$STATE" || exit 1
[ ! -e "$STATE/quarantined" ] || exit 0
[ -e "$STATE/stop" ] && rm -f "$STATE/stop"
# supervisor owns duplicate prevention and crash backoff; native daemon owns capture recovery.
if command -v setsid >/dev/null 2>&1; then
    AUDIOCAST_APP_DIR="$APP" AUDIOCAST_STATE_DIR="$STATE" nohup setsid sh "$APP/service/startup/supervisor.sh" </dev/null >>"$STATE/supervisor.log" 2>&1 &
else
    AUDIOCAST_APP_DIR="$APP" AUDIOCAST_STATE_DIR="$STATE" nohup sh "$APP/service/startup/supervisor.sh" </dev/null >>"$STATE/supervisor.log" 2>&1 &
fi
exit 0
