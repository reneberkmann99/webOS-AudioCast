# Release notes — 0.1.0 (engineering prerelease)

**This is a hardware-unqualified engineering prerelease.** It has not been installed on or validated against a physical LG television. Do not treat it as a stable or TV-qualified release.

## Implemented

- Native C++11 engine: explicit-ALSA capture backend, drop-oldest bounded ring, no gain/resample/route changes.
- RTP/L16 (network-order, MTU-bounded fragmentation, seq/timestamp wrap, marker/discontinuity), compound RTCP SR/SDES + BYE, SDP endpoint.
- HTTP raw PCM (`application/octet-stream`, chunked, explicit S16_LE/S32_LE, multiclient with bounded per-client queues), `/metadata` and `/stream.sdp`.
- Authenticated loopback-only control API (`/v1/status|devices|config|start|stop|test-capture|test-connection|suspend|resume|logs|diagnostics`), distinct stream bearer, no recording.
- Unprivileged ES5 TV web app: remote spatial navigation, on-screen editor, real level meters, honest unknown states, source-confirmation flow, no fabricated data.
- Detached root supervisor: identity-guarded PID handling, duplicate prevention, bounded crash backoff + quarantine, symlink Homebrew boot hook, uninstall-aware shutdown, `setsid` where available.
- Reference Python receiver: RTP and HTTP, loss/reorder/wrap accounting, bounded jitter buffer, optional sounddevice playback, explicit-only WAV recording, optional lighting feature hook.
- Reproducible pinned-tool ARM packaging, deterministic IPK normalization/validation, source release, GitHub Actions prerelease workflow.

## Tested (host, non-TV)

- 40 host tests across native daemon, ALSA ABI double, receiver, and real-process supervisor lifecycle.
- Browser integration against the real native daemon using explicit ALSA/Homebrew doubles (11 checks).
- Synthetic streaming benchmark across 5/10/20 ms blocks and low-latency/stable profiles: zero queue drops and zero RTP send errors over 30 s each.

See `docs/validation.md` for the exact verified/unverified boundary and reproduction commands.

## Not implemented / not verified

- No physical TV capture, firmware ALSA/loader, or Homebrew lifecycle test (see `docs/compatibility.md` worksheet).
- No automatic standby-event subscription, adaptive clock correction, Opus/RTP, PulseAudio monitor, >2-channel or compressed passthrough.
- RTP/HTTP transport is intentionally unencrypted; trusted LAN/VPN only.
- No automatic central-repository submission (see `docs/publication.md`).

## Breaking/behavior notes

- Start requires both an explicit capture device and explicit playback confirmation; opening or nonzero samples never auto-verify a source.
- Configuration changes (including device/format) invalidate prior confirmation and require the engine to be idle.
- `hw:0,13` is not a built-in default; it was one externally reported configuration and must be discovered/validated per TV.
