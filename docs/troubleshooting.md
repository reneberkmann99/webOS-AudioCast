# Troubleshooting

## Homebrew unavailable / bootstrap fails

Developer Mode is not root. Check Homebrew Channel's root status, then run its documented `exec` health query from `diagnose.sh`. AudioCast's helper checks `id -u`; a successful Luna call alone is not proof of root. Confirm the installed app path and executable ARM binary. A desktop browser intentionally shows an error and zero meters, not demo status.

Protected diagnostics/settings: `/var/lib/webos-audiocast`. Native event log: GUI Diagnostics or authenticated `/v1/logs`. Startup fatal errors: `daemon.stderr`/`.1`; supervisor events: `supervisor.log`. Do not share `credentials.json` or bootstrap replies.

## Exec format / loader / missing library

Current package contains ARMv7NEON EABI/soft-float,32-bit. Inspect `uname -m` and available32-bit runtime. A64-only firmware without qualified compatibility runtime is unsupported by this binary. `libasound.so.2` must actually exist and export the expected ABI. Do not copy random vendor libraries or replace LG system libraries. Host cross-build/QEMU against Debian libc is not LG loader validation.

## No capture devices / no playback tap

`/proc/asound/cards`, `/proc/asound/pcm`, `arecord -l`/`-L` (when present) inventory only. CAPTURE_TEST is a hint, never proof. `hw:0,13` was one externally reported Realtek configuration, not a default or compatibility promise. Manually select only a known candidate and test it. If nothing captures authorized TV playback, report unsupported for that model/firmware/source. **Never select default/microphone input or change routing/mixers to fake success.**

## Device opens but meters are silent

Actual silence is reported as silence. Check known authorized content, selected source, output mode, correct native sample format/rate/channels and protected-path restrictions. Nonzero samples still need correlation/listening verification; noise may be an unrelated input. Mute/volume behavior relative to the tap is model-specific, not controlled by this app. No source is automatically marked working just because it opens or produces samples.

## Unsupported rate/format / busy device

Explicit native rate must match negotiation; no automatic resampler. S32_LE is HTTP-only, RTP is L16. Only mono/stereo,32/44.1/48/96k supported by engine; hardware can support fewer. Capability sets are independent constraints, not a promise that all combinations work. Stop and wait for resource release before Apply. Capture contention and vendor AI processing need model-specific investigation; AudioCast will not disable another feature or reroute output automatically.

## Start accepted, no receiver audio

Read actual engine state (`starting`, `capturing`, `recovering`) and signal, not command acknowledgement. Verify receiver rate/channels/PT96 and inbound UDP firewall on the receiver computer, correct private IP and port. RTP bytes are big-endian; HTTP bytes little-endian. RTP receiver count is unknown; UDP socket/resolve success cannot detect a listening receiver. Run the reference receiver and inspect real PCM levels/sample identity. HTTP requires modeHTTP/Both, explicit LAN enable, and stream bearer (not control bearer). Control7071 is intentionally not reachable from LAN.

## Gaps, latency, Wi-Fi problems

Inspect packet gaps, late/missing frames, queue drops, send errors, HTTP slow-client disconnects and optional playback underruns. Try wired LAN; compare5/10/20ms block sizes and20–60ms receiver jitter targets. Stable ALSA buffering adds potential delay, not a guaranteed cure. Packet sizes remain MTU-bounded. A slow HTTP client is disconnected without affecting others; reconnect it. Reference HTTP CLI currently exits/requires relaunch after a broken stream. No packet-duration/ping value is a measured end-to-end latency.

## Sleep / source switching / application exit

Closing AudioCast's GUI leaves the detached service running. Host-tested; real webOS lifecycle must be qualified. Automatic standby LS2 subscription is not implemented. Use **Release for standby** / Resume or Stop. Device errors close/reopen with bounded backoff; if a vendor tap stays open during standby that may not trigger recovery. Source/output changes require source-specific real-TV verification; no assumption is made from a still-running process. Sustained clock drift can cause receiver silence insertion/drops; no implicit resampler is applied.

## Crash loops / stale lock / app upgrade

Eight short process failures quarantine startup. Inspect fatal stderr, fix real cause, then Reconnect explicitly. Missing-owner supervisor locks are not blindly deleted during potential concurrent startup. If stale, verify no matching processes first; remove only the stale AudioCast lock. Native flock prevents duplicate daemons. Before an upgrade, stop the service, install the new IPK, reopen. A live old process is not assumed to restart on atomic package replacement. Keep the prior IPK/config backup for rollback.

## Meter or status stuck

UI reads actual daemon state every~400ms without overlapping polls. Network disconnect resets meters and reports unknown. Check GETstatus and last error; reconnect the local service if needed. Driver bugs can block beyond userspace's nonblocking requests; no universal kernel guarantee is claimed. Use the fixed whole-service stop helper and inspect system logs rather than modifying firmware.

## Diagnostic export unsupported by TV browser

Sanitized JSON is displayed regardless of download support. Use the bundled SSH `tools/diagnose.sh` for a reliable file export. No recording or private token is included. Review serial/model/network identifiers before sharing.
