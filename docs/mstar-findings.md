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

## What IS the capture path (confirmed mechanism, not yet implemented)

The vendor playback-capture API is **`libdile_audio_pcm.so`** (LG DILE "Device Interface Layer Engine" audio PCM), exported symbols:

```text
DILE_AUDIO_PCM_RegSendPCMCallback   # register a callback the HAL calls with decoded PCM
DILE_AUDIO_PCM_StartUpload          # begin "upload" (HAL → your callback)
DILE_AUDIO_PCM_StopUpload
DILE_AUDIO_PCM_SetChannel           # (present on some firmware builds)
DILE_AUDIO_PCM_SetVolume
# each has a matching HAL_AUDIO_PCM_* implementation in libhal_audio_direct.so
```

This is confirmed by **two shipping consumers** that both capture the TV's own playback audio:

- **`/usr/sbin/btaudiosrc`** — the A2DP **Bluetooth audio source** (streams TV audio to BT headphones). It links `libdile_audio_pcm` and calls `RegSendPCMCallback`/`StartUpload`/`StopUpload`. Bluetooth audio-out is literally "capture system audio and re-encode it", so this is the exact capability AudioCast needs.
- **`/usr/sbin/acr2`** — Samba TV **ACR** (automatic content recognition; `livePlus`). Uses `libdile_audio_pcm` for audio fingerprinting plus `libcapture.so` (`CAPTURE_CreateContext`/`CAPTURE_AcquireVtResource`) for video — the same video capture PicCap uses. (ACR is `off` on this unit.)

### Why it is not implemented yet

The C signatures are **not in any public header** (`webos-userland` has `dile_audio_direct.h`, which is a *playback-injection* API, but no `dile_audio_pcm.h`). Public dumps (`webosbrew/dev-toolbox-cli`, `Powback/LGTV`) carry only symbol **names**. To implement a native MStar backend we must recover, by disassembling `btaudiosrc` / `libdile_audio_pcm.so` (ARM32):

1. The `RegSendPCMCallback` callback prototype — PCM buffer pointer, byte length, and whether format/rate/channels are passed or fixed (`btaudiosrc` targets A2DP, commonly 44.1/48 kHz S16 stereo).
2. `StartUpload` parameters (channel/format selection, or a context handle from a prior call).
3. Threading/lifetime rules and whether only one consumer may `StartUpload` at a time (contention with `btaudiosrc`/`acr2`).

### Proposed native backend (pending owner go-ahead — it opens the vendor HAL)

A new `daemon/backends/dile_pcm.cpp` backend that `dlopen`s `libdile_audio_pcm.so.1`, registers a callback, `StartUpload`s, and feeds the existing bounded ring → RTP/HTTP pipeline unchanged. This must be developed with **single, strictly time-boxed on-device trials** and an external watchdog that force-reboots if a call wedges (because, like the ALSA path, a bad HAL call could hang uninterruptibly). It must coexist with / yield to `btaudiosrc` and `acr2`. **Not attempted yet: running code that calls `StartUpload` is not a read-only operation and needs explicit approval.**

## Guards shipped this session

- `daemon/common.cpp::capture_hazard()` + `discover()` flag any `mad_pcm` / `MStar MAD` capture PCM as `safe_to_open:false` with an `unsafe_reason`, set `soc_mstar_mad:true`, and refuse to open them in the ALSA backend and `--probe-device`.
- `scripts/diagnose.sh` refuses to open an MStar MAD capture device even for `--dump-hw-params`.
- Test `tests/test_alsa_backend.py::test_mstar_mad_capture_is_blocked_before_touching_kernel` (injects a synthetic `/proc/asound/pcm` via `AUDIOCAST_PROC_PCM`).
