# webOS AudioCast

Capture real system playback audio from a rooted LG webOS TV and stream it to another device on your local network — for music-reactive lighting, audio visualization, authorized recording, or playback on another device.

> **Status: hardware-unqualified engineering prerelease.** The native engine, network streaming, reference receiver, TV interface, supervisor, and IPK are implemented and covered by host-side automated tests. **No physical LG TV capture has been tested.** TV playback capture, real firmware ALSA behavior, and model compatibility remain unverified until the on-device worksheet in `docs/compatibility.md` is completed.

```text
TV playback ─▶ vendor capture tap (explicit hw:C,D) ─▶ native ALSA read ─▶ bounded ring
                  (never microphone / default / mixer writes)              │
                                                                    RTP/L16 (UDP)
                                                                    RTCP + SDP
                                                                    HTTP raw PCM (7070)
                                                                          ▼
                                                                   receiver.py / your engine
```

## What it is / is not

- Captures a **copy** of playback audio where the firmware exposes a supported tap; TV speakers/output continue normally. This app never modifies routing, mixer, gain, or volume.
- Runs as a detached root supervisor so streaming survives closing the TV app; uninstall stops the service and leaves only a symlink, not an executable boot script.
- Requires an already-rooted TV with Homebrew Channel. It does not root TVs, patch firmware, or bypass DRM. Protected playback may yield silence or deny capture by design.
- Captures from an **explicit** `hw:CARD,DEVICE` only. No `default`/microphone fallback, no ALSA loopback installation, no hidden resampling, no TV-side recording. `hw:0,13` (MARS CAPTURE_TEST) was an externally reported Realtek/webOS 7.4.0 example, **not a universal default**.

## Quick start

Build (Linux x86_64 host, Python ≥3.12, C++ compiler; Node ≥20 only for dev tests):

```sh
./scripts/build.sh test       # native build + 40 host tests (daemon/ALSA-ABI/receiver/supervisor)
npm ci                        # dev-only; then: npm run test:ui  (browser vs real native API)
./scripts/build.sh package    # pinned SDK fetch → ARMv7 IPK + Homebrew manifest
```

Install/validate on a rooted TV: follow `docs/installation.md`, then fill the worksheet in `docs/compatibility.md` **before** relying on any model. A prebuilt `dist/org.webosbrew.audiocast_0.1.0_arm.ipk` and its `dist/SHA256SUMS` checksum are included in the engineering release.

Receive:

```sh
# RTP (default, low-latency):
python3 receiver/receiver.py rtp --port 5004 --rate 48000 --channels 2
# HTTP (explicit LAN enable + stream bearer):
python3 receiver/receiver.py http --url http://TV_IP:7070/audio --token-file stream-token.txt
```

## Repository layout

```text
app/            unprivileged ES5 webOS TV app (remote navigation, real API, no mock status)
daemon/         native C++11 engine: capture, backends, network (RTP/RTCP/HTTP), control
service/        Homebrew startup supervisor + fixed-action manage/boot scripts
receiver/       cross-platform Python reference receiver (+ optional lighting hook)
scripts/        diagnose.sh, build.sh, package.py, ipk.py, fetch-tools.py, deploy.sh, soak.py
tests/          daemon/ALSA-ABI/receiver/supervisor integration + UI browser tests
docs/           architecture, protocols, installation, receiver, compatibility, security, …
third_party/    pinned nlohmann/json single header (MIT) — nothing else vendored
```

## Documentation index

- `docs/architecture.md` — process/privilege boundaries, capture, buffering, lifecycle, security
- `docs/protocols.md` — RTP/L16 + RTCP, SDP, HTTP raw PCM, authenticated localhost control API
- `docs/installation.md` — build, install, playback validation, receiver, upgrade/rollback/removal
- `docs/receiver.md` — receiver CLI, recording, lighting/FFT hook, latency measurement
- `docs/compatibility.md` — qualification matrix and required on-TV test worksheet
- `docs/security.md` — trust boundaries, transport limits, credentials, privacy
- `docs/troubleshooting.md` — failure diagnosis without firmware modification
- `docs/research.md` — audio-architecture feasibility findings and source attribution
- `docs/validation.md` — what was actually tested vs unverified, how to reproduce

## Provenance & license

New MIT-licensed code, developed with substantial AI assistance. No LG/vendor code or libraries are redistributed; native ALSA access uses the TV's own `libasound.so.2`. Full attribution and the central-repository submission status (not yet submitted; requires human TV testing/review) are in `NOTICE.md`, `docs/research.md`, and `docs/publication.md`.

## Honest limitations

- RTP/L16 is unencrypted and unauthenticated; HTTP is cleartext. Trusted LAN only.
- UDP "reachability" and sample-clock timestamps cannot prove delivery or end-to-end latency.
- Automatic standby-event subscription, adaptive clock correction, Opus, PulseAudio monitor, multichannel/compressed passthrough, and 64-bit-only TVs are **not** supported in 0.1.
