# Real-hardware findings — LG MStar webOS TV (2026-10-21)

On-device investigation over SSH (`root@10.10.40.104`, authorized by the owner). This supersedes the reference project's assumptions for MStar-class LG TVs. It is specific to the tested unit; treat other models as unverified.

## Unit under test

- CPU: ARMv7l (3 cores), NEON/VFPv4. **SoC family: MStar** (kernel `4.4.84-636.glacier.26`, webOS build `20250429094550`).
- Already rooted, Homebrew Channel present; PicCap (`hyperion-webos`) and the HyperHDR loader were installed and running.
- ALSA driver: `mdrv_alsa_audio` + `mhal_alsa_audio` → two cards, **"MStar MAD"** (MStar MPEG Audio Decoder) No.00/No.01. `/dev/mad` present (world-writable).
- Sound server: PulseAudio 9.0 (`--system`), default sink `pcm_output` (`module-alsa-sink`, `hw:0,6`, 48 kHz), default source `pcm_output.monitor`, plus many `module-null-sink`s (`pmedia`, `pnetflix`, `pdefaultapp`, …). LG `audiod` owns routing.

## What does NOT work (verified)

1. **No `hw:0,13` / MARS CAPTURE_TEST.** That is Realtek-specific. This TV has no such device; `arecord -L` lists only `null`, `pulse`, `sysdefault`.
2. **Media audio bypasses PulseAudio.** With YouTube actively playing music, every Pulse sink stayed `SUSPENDED` with **zero sink-inputs**, and every monitor source (`pcm_output.monitor`, `pmedia.monitor`, `pnetflix.monitor`, …) read **pure silence**. App/media audio is decoded by the MStar MAD hardware and never enters a capturable Pulse graph.
3. **The MStar MAD ALSA "capture" PCMs are hazardous and empty.** `hw:0,0/0,1/1,0/1,1` advertise S16_LE/48k/stereo but:
   - When idle they return `read error: Input/output error`.
   - When opened during playback they deliver **0 bytes** and the process **wedges in uninterruptible D-state** inside `snd_pcm_open` / `mdrv_alsa_capture_close`. `kill -9` cannot reap them; only a reboot clears them. (I triggered this during testing; a reboot restored the TV.)
   - They are a decoder path, not a playback tap. **The daemon and `diagnose.sh` now hard-block these devices** (`capture_hazard()`), overridable only with `AUDIOCAST_ALLOW_MSTAR_MAD_CAPTURE=1`.

## Working playback-correlated capture path (experimental)

A standalone DILE probe now captures a signal that follows owner-controlled YouTube playback:
RMS **0.148537 playing → 0.000010 paused → 0.135366 resumed**. Each five-second trial delivered
208 buffers / 958464 bytes. The missing initialization was **PCM-upload gain 127**, also used by
LG's `acr2` consumer; the original gain `12+0` effectively attenuated the upload to silence.
Every trial restored `12+0` and verified it after `StopUpload`. No routing, speaker-volume or
ALSA changes, saved audio, or receiver playback were involved. The owner authorized further
bounded tests up to 30 seconds each, not unlimited or boot-time streaming.

The bounded native backend is integrated and verified through the installed TV daemon's capture-only
API, with zero observed capture discontinuities and verified gain restoration. Audible receiver output
was explicitly deferred by the owner; other sources/output modes, coexistence and long-running
reliability remain unverified. See `docs/capture-fix-20261021.md`.
Detailed signatures and evidence: `docs/dile-pcm-api.md`.

## Vendor interface

The vendor playback-capture API is **`libdile_audio_pcm.so`** (LG DILE "Device Interface Layer Engine" audio PCM), exported symbols:

```text
DILE_AUDIO_PCM_RegSendPCMCallback   # register a callback the HAL calls with decoded PCM
DILE_AUDIO_PCM_StartUpload          # begin "upload" (HAL → your callback)
DILE_AUDIO_PCM_StopUpload
DILE_AUDIO_PCM_SetChannel           # (present on some firmware builds)
DILE_AUDIO_PCM_SetVolume
# this firmware implements HAL_AUDIO_PCM_* in libhal_msd96alvx.so
```

This is confirmed by **two shipping consumers** that both capture the TV's own playback audio:

- **`/usr/sbin/btaudiosrc`** — the A2DP **Bluetooth audio source** (streams TV audio to BT headphones). It links `libdile_audio_pcm` and calls `RegSendPCMCallback`/`StartUpload`/`StopUpload`. Bluetooth audio-out is literally "capture system audio and re-encode it", so this is the exact capability AudioCast needs.
- **`/usr/sbin/acr2`** — Samba TV **ACR** (automatic content recognition; `livePlus`). Uses `libdile_audio_pcm` for audio fingerprinting plus `libcapture.so` (`CAPTURE_CreateContext`/`CAPTURE_AcquireVtResource`) for video — the same video capture PicCap uses. (ACR is `off` on this unit.)

### Recovered ABI and remaining engineering work

No public `dile_audio_pcm.h` was found; the signatures came from static ARM analysis of this
TV's binaries, not copied vendor source. Working calls are `StartUpload(0)`,
`RegSendPCMCallback(0, callback)`, `SetVolume(uint16_t)` and `StopUpload(0)`; the callback is
`int callback(const void *pcm, int bytes)`. It delivers 4608-byte buffers at a rate consistent
with interleaved S16_LE / 48 kHz / stereo. The DILE gain wrapper fixes the HAL channel to zero.

The experimental backend must be explicit opt-in, refuse other firmware/formats, keep callback
work bounded, restore upload gain on stop/errors and cap capture to the authorized 30 seconds.
A timeout cannot recover an uninterruptible kernel hang; reboot recovery remains a risk, not a
claim of safety. Coexistence with active Bluetooth sound-out and ACR is not qualified. Never
infer that a sleeping `btaudiosrc` process alone means its upload is active. No automatic routing,
permissions/ACL changes or microphone/default-input fallback is permitted.

## Guards shipped this session

- `daemon/common.cpp::capture_hazard()` + `discover()` flag any `mad_pcm` / `MStar MAD` capture PCM as `safe_to_open:false` with an `unsafe_reason`, set `soc_mstar_mad:true`, and refuse to open them in the ALSA backend and `--probe-device`.
- `scripts/diagnose.sh` refuses to open an MStar MAD capture device even for `--dump-hw-params`.
- Test `tests/test_alsa_backend.py::test_mstar_mad_capture_is_blocked_before_touching_kernel` (injects a synthetic `/proc/asound/pcm` via `AUDIOCAST_PROC_PCM`).
