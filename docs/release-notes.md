# Release notes — 0.1.1 (experimental engineering prerelease)

**Not a stable or broadly qualified release.** Installation, GUI, service startup and playback-correlated YouTube capture have been tested on one MStar ARMv7 LG TV. Other source/output combinations, audible receiver output and long-running reliability remain unqualified. MStar capture is explicitly experimental and limited to 30 seconds per manual start.

## Implemented

- Native C++11 engine: explicit ALSA backend plus exact-firmware MStar DILE playback upload. DILE needs separate temporary-upload-gain consent, fixed S16_LE/48 kHz/stereo, private recovery records and manual start; it restores the original gain on stop/error and fails closed without automatic retry. No routing, speaker-volume or resampling changes.
- RTP/L16 (network-order, MTU-bounded fragmentation, seq/timestamp wrap, marker/discontinuity), compound RTCP SR/SDES + BYE, SDP endpoint.
- HTTP raw PCM (`application/octet-stream`, chunked, explicit S16_LE/S32_LE, multiclient with bounded per-client queues), `/metadata` and `/stream.sdp`.
- Authenticated loopback-only control API (`/v1/status|devices|config|start|stop|test-capture|test-connection|suspend|resume|logs|diagnostics`), distinct stream bearer, no recording.
- Unprivileged ES5 TV web app: remote spatial navigation, on-screen editor, real level meters, honest unknown states, source-confirmation flow, no fabricated data.
- Detached root supervisor: identity-guarded PID handling, duplicate prevention, bounded crash backoff + quarantine, symlink Homebrew boot hook, uninstall-aware shutdown, `setsid` where available.
- Reference Python receiver: RTP and HTTP, loss/reorder/wrap accounting, bounded jitter buffer, optional sounddevice playback, explicit-only WAV recording, optional lighting feature hook.
- Reproducible pinned-tool ARM packaging, deterministic IPK normalization/validation, source release, GitHub Actions prerelease workflow.

## Tested (host, non-TV)

- Native daemon, ALSA and DILE ABI doubles, receiver, and real-process supervisor regression suites, including interrupted-gain recovery and quarantine.
- Browser integration against the real native daemon using explicit ALSA/Homebrew doubles, plus frontend-only DILE consent/format/autostart controls. See current counts in `docs/validation.md`.
- Synthetic streaming benchmark across 5/10/20 ms blocks and low-latency/stable profiles: zero queue drops and zero RTP send errors over 30 s each.

See `docs/validation.md` for the exact verified/unverified boundary and reproduction commands.

## Not implemented / not verified

- No complete physical source/output matrix, audible TV-to-receiver acceptance, or long-running qualification (see `docs/compatibility.md`). YouTube pause/resume correlation is demonstrated only on the inspected MStar firmware.
- No automatic standby-event subscription, adaptive clock correction, Opus/RTP, PulseAudio monitor, >2-channel or compressed passthrough.
- RTP/HTTP transport is intentionally unencrypted; trusted LAN/VPN only.
- No automatic central-repository submission (see `docs/publication.md`).

## Breaking/behavior notes

- Start requires an explicit capture device and playback confirmation; DILE additionally requires `dile_gain_confirmed`, selector `mstar-upload:0`, and manual start. Its 30-second limit cannot be reset by retry or suspend/resume. Opening or nonzero samples never auto-verify a source.
- Configuration changes (including device/format) invalidate prior confirmation and require the engine to be idle.
- `hw:0,13` is not a built-in default; it was one externally reported configuration and must be discovered/validated per TV.
