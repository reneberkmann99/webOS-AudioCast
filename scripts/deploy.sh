#!/bin/sh
# SPDX-License-Identifier: MIT
# Explicitly installs only; never auto-roots, changes routing, or starts streaming.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
[ "$#" -ge 1 ] && [ "$#" -le 2 ] || { echo "Usage: $0 CONFIGURED_DEVICE [IPK]" >&2; exit 2; }
DEVICE=$1
IPK=${2:-$ROOT/dist/org.webosbrew.audiocast_$(cat "$ROOT/VERSION")_arm.ipk}
[ -f "$IPK" ] || { echo 'IPK missing: build with scripts/build.sh package' >&2; exit 1; }
python3 "$ROOT/scripts/ipk.py" --validate-only "$IPK"
ARES=${ARES_INSTALL:-$ROOT/.tools/ares/ares-cli-rs-v0.7.0-linux-x86_64/ares-install}
[ -x "$ARES" ] || { echo 'Pinned ares-install missing: run scripts/fetch-tools.py or set ARES_INSTALL to a trusted installed tool' >&2; exit 1; }
echo "Installing AudioCast to configured device $DEVICE. Stop any existing AudioCast service before upgrades."
"$ARES" --device "$DEVICE" "$IPK"
echo 'Install command completed; inspect its actual success output. Open AudioCast manually to configure/test. No streaming was requested by this script.'
