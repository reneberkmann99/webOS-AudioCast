# What was tested, and what was not

This document is the boundary between verified behavior and unverified claims. Host tests are separated from the real-TV observations below. Neither synthetic PCM nor a running daemon is proof of TV playback capture.

## Verified by automated tests (host, non-TV)

Current integrated build: **56 host tests**, **17 real-native-API browser checks** and
**11 frontend-only DILE control checks**, all passing. Actual TV capture evidence is separate below.

Run with:

```sh
./scripts/build.sh test      # native build + Python unittest suites
npm run test:ui              # browser against the real native daemon (explicit doubles only)
python3 scripts/soak.py      # synthetic host streaming benchmark
```

| Area | Evidence | Status |
|---|---|---|
| Native engine builds/links | `scripts/build.sh native` g++ clean under `-Wall -Wextra -Werror -Wpedantic` | Tested |
| ARM cross-compilation/runtime | `scripts/build.sh webos` with pinned SDK; ARM32 binary self-test and bounded integrated DILE probe run on the real TV | Tested on one firmware; other firmware unqualified |
| RTP/L16 wire format | Big-endian, PT96, seq/timestamp/SSRC, marker, MTU-bounded fragmentation, timestamp wrap | Tested |
| RTCP SR/SDES/BYE | Compound sender report, SDES CNAME, BYE on teardown | Tested |
| HTTP raw PCM | Chunked framing across arbitrary read boundaries, S16_LE/S32_LE, mono/stereo, multiclient, slow-client isolation | Tested |
| Authentication | Distinct control/stream bearers, 401 without token, token-free diagnostics/logs, 0600/0700 state | Tested |
| Config validation | Rejects unknown fields, invalid ports/rates/privates/device strings, S32_LE-on-RTP, atomic persistence | Tested |
| ALSA error handling | xrun re-prepare, ENODEV backoff, wrong native rate rejection, suspend EAGAIN non-looping release, 3s empty-read timeout, partial frames, silence ≠ signal | Tested via ABI double |
| Capture consent | Explicit device + playback confirmation; separate DILE upload-gain consent; fixed DILE format, no autostart, total 30-second cap, no automatic HAL retry; capture-only test never streams | Tested via explicit ABI doubles |
| Supervisor lifecycle | Duplicate prevention, PID-identity guarding, uninstall stops child, bounded crash backoff + quarantine, foreign-file preservation, symlink boot hook, action whitelist | Tested (host, real processes) |
| Receiver | RTP/HTTP end-to-end against native daemon, loss/reorder/wrap/jitter bound, WAV record alignment, explicit-only recording, feature hook | Tested |
| UI | File-URI CSS/scripts, both Homebrew bridge APIs, remote editor, unsaved-edit preservation, IP/port persistence, explicit confirmation, actual synthetic RTP receiver packets, independent boot startup, diagnostics, unsafe-source blocking, 1280/1920 widths | Tested (real native API; ALSA + Homebrew doubles) |
| Performance | Four 30s synthetic profiles; queue drops 0, RTP send errors 0, MTU max 1200B, ~7–15% CPU on host | Tested (synthetic only) |

`tests/` uses compiled ALSA and DILE ABI **doubles** and a `PalmServiceBridge` **double**; these are explicit, never packaged, and never evidence of TV behavior. The DILE fixture requires both an explicit test-library path and `--allow-test-source`; normal execution verifies the inspected real library contents and symbol offset. `build/proofs/` retains UI screenshots and the benchmark JSON.

## Real-TV observations (one MStar ARMv7 TV, 2026-10-21)

- ARM binary runs and passes its self-test on the real firmware.
- IPK installation and app launch work through configured ares tools.
- The background daemon was observed starting via Homebrew init.d after a full reboot, with streaming disabled.
- The original blank/unstyled GUI was traced to actual TV logs: Chrome 53 refused every local stylesheet, image and script under CSP `'self'` (opaque file origins). The corrected policy explicitly permits local file assets, never remote scripts.
- The direct PalmServiceBridge call requires a full `/exec` URI and flat `{command: ...}` payload; the optional webOS.js SDK uses a base URI and `{method,parameters}` options. These are distinct APIs.
- **YouTube playback-correlated capture works through the vendor DILE path.** With temporary upload gain 127, RMS was 0.148537 playing, 0.000010 owner-paused, and 0.135366 resumed. The original upload gain 12 was verified restored after every trial. The native integrated backend also returned nonzero stereo PCM on the TV; neither installation nor daemon liveness is being substituted for this capture evidence. No audio was saved or sent to a receiver in these trials. Pulse monitors remain silent; hazardous MAD ALSA devices remain blocked. See `docs/dile-pcm-api.md` and the capture-fix report.

Actual GUI acceptance is recorded separately in `docs/gui-fix-20261021.md` and `build/proofs/tv/` when performed. These observations apply only to this TV/firmware.

## Not qualified (requires further real-TV testing)

- Media capture beyond YouTube on the inspected MStar unit/firmware, or audible receiver output. The Realtek MARS CAPTURE_TEST report remains externally sourced and unreproduced.
- Whether opening a capture PCM is non-invasive on a given TV, or that speakers/routing truly remain unchanged.
- Real source/output behavior (internal apps, HDMI, tuner, Bluetooth, optical, ARC/eARC), mute/volume interaction, source switching, protected playback.
- General firmware/ALSA compatibility across TVs and vendor driver edge cases; the tested MStar MAD devices are known hazardous, not merely unverified.
- General Homebrew lifecycle across firmware, GUI close while real playback streams, and real app uninstall semantics; boot-service startup is observed on the one tested TV.
- TV standby/wake transitions and automatic power-event recovery.
- Multi-hour stability, actual capture-to-receiver latency, and drift behavior on real clocks.
- Rollback/removal and Dev Manager installation; ares installation has been observed on the tested TV.

## How to close the gap

1. Follow `docs/installation.md` on an authorized rooted TV; collect inventory without opening capture first.
2. Test known authorized playback; correlate meter and reference-receiver samples; confirm speakers remain normal.
3. Record each source/output cell in `docs/compatibility.md`.
4. Only then reconsider `docs/publication.md` and any stable release.

Do not reclassify an untested cell as supported based on this repository's host test results.
