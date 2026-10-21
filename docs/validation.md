# What was tested, and what was not

This document is the boundary between verified behavior and unverified claims. Everything below is reproducible from this repository; nothing here asserts a result from a physical LG television.

## Verified by automated tests (host, non-TV)

Run with:

```sh
./scripts/build.sh test      # native build + Python unittest suites
npm run test:ui              # browser against the real native daemon (explicit doubles only)
python3 scripts/soak.py      # synthetic host streaming benchmark
```

| Area | Evidence | Status |
|---|---|---|
| Native engine builds/links | `scripts/build.sh native` g++ clean under `-Wall -Wextra -Werror -Wpedantic` | Tested |
| ARM cross-compilation | `scripts/build.sh webos` with pinned 2026.08-webos SDK; `readelf` ARM32; `--self-test` passes under QEMU using a Debian ARM libc | Tested (loader/runtime not LG) |
| RTP/L16 wire format | Big-endian, PT96, seq/timestamp/SSRC, marker, MTU-bounded fragmentation, timestamp wrap | Tested |
| RTCP SR/SDES/BYE | Compound sender report, SDES CNAME, BYE on teardown | Tested |
| HTTP raw PCM | Chunked framing across arbitrary read boundaries, S16_LE/S32_LE, mono/stereo, multiclient, slow-client isolation | Tested |
| Authentication | Distinct control/stream bearers, 401 without token, token-free diagnostics/logs, 0600/0700 state | Tested |
| Config validation | Rejects unknown fields, invalid ports/rates/privates/device strings, S32_LE-on-RTP, atomic persistence | Tested |
| ALSA error handling | xrun re-prepare, ENODEV backoff, wrong native rate rejection, suspend EAGAIN non-looping release, 3s empty-read timeout, partial frames, silence ≠ signal | Tested via ABI double |
| Capture consent | Start requires explicit device + playback confirmation; device/format change invalidates; test-capture never streams and expires | Tested |
| Supervisor lifecycle | Duplicate prevention, PID-identity guarding, uninstall stops child, bounded crash backoff + quarantine, foreign-file preservation, symlink boot hook, action whitelist | Tested (host, real processes) |
| Receiver | RTP/HTTP end-to-end against native daemon, loss/reorder/wrap/jitter bound, WAV record alignment, explicit-only recording, feature hook | Tested |
| UI | Remote navigation, on-screen editor, real meters from synthetic PCM, honest UDP probe text, app-close leaves capture running, 1280px no overflow, no-Homebrew error, zero console errors | Tested (ALSA + Homebrew doubles) |
| Performance | Four 30s synthetic profiles; queue drops 0, RTP send errors 0, MTU max 1200B, ~7–15% CPU on host | Tested (synthetic only) |

`tests/` uses a compiled `libasound.so.2` **double** and a `PalmServiceBridge` **double**; these are explicit, never packaged, and never evidence of TV behavior. `build/proofs/` retains UI screenshots and the benchmark JSON.

## Not verified (requires a physical rooted LG TV)

- Any actual playback capture on any TV model, SoC, or firmware. The MARS CAPTURE_TEST report was externally sourced; AudioCast has not reproduced it.
- Whether opening a capture PCM is non-invasive on a given TV, or that speakers/routing truly remain unchanged.
- Real source/output behavior (internal apps, HDMI, tuner, Bluetooth, optical, ARC/eARC), mute/volume interaction, source switching, protected playback.
- Real firmware `libasound.so.2` ABI/loader compatibility and vendor driver edge cases (uninterruptible ioctls).
- Homebrew `exec`/`spawn` lifetime, `nohup`/`setsid` persistence, init.d `run-parts` invocation, and app uninstall process semantics on webOS.
- TV standby/wake transitions and automatic power-event recovery.
- Multi-hour stability, actual capture-to-receiver latency, and drift behavior on real clocks.
- Install/upgrade/rollback via Dev Manager / ares on a TV.

## How to close the gap

1. Follow `docs/installation.md` on an authorized rooted TV; collect inventory without opening capture first.
2. Test known authorized playback; correlate meter and reference-receiver samples; confirm speakers remain normal.
3. Record each source/output cell in `docs/compatibility.md`.
4. Only then reconsider `docs/publication.md` and any stable release.

Do not reclassify an untested cell as supported based on this repository's host test results.
