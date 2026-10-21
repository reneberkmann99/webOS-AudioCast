# DILE Audio PCM capture API — recovered signature (LG MStar)

Recovered 2026-10-21 by static analysis (host-side objdump) of firmware binaries pulled read-only
from a real LG MStar webOS TV (`libdile_audio_pcm.so`, `libhal_msd96alvx.so`, `btaudiosrc`).
This is reverse-engineered knowledge, not LG documentation; treat as provisional until verified
by an on-device trial.

## Library chain

`libdile_audio_pcm.so.1` is a thin stub (16-byte functions) that forwards unchanged to
`HAL_AUDIO_PCM_*`, which are implemented in **`libhal_msd96alvx.so`** (the MStar board HAL).

## Exported API (libdile_audio_pcm.so)

Return convention (confirmed from stub disassembly): `0` = success, `-1` = failure
(DILE_STATUS_T). The stub forwards `r0`/`r1` verbatim to the HAL and maps nonzero HAL return
to `-1`.

```c
// All confirmed present on the tested firmware build (webOS 20250429094550):
int DILE_AUDIO_PCM_RegSendPCMCallback(int channel, int (*callback)(const void *pcm, int bytes));
int DILE_AUDIO_PCM_StartUpload(int channel);
int DILE_AUDIO_PCM_StopUpload(int channel);
int DILE_AUDIO_PCM_SetVolume(unsigned int volume16);   // 16-bit value; stub repacks two bytes

// Present in HAL (libhal_msd96alvx.so) but NOT re-exported by the DILE stub on this build:
//   HAL_AUDIO_PCM_SetInfo / GetInfo / SetDataCount / RegUploadCallBack
//   (a newer firmware build also exports DILE_AUDIO_PCM_SetChannel)
```

## Confirmed call sequence (from `btaudiosrc`, the A2DP Bluetooth audio source)

```c
// 1. register the callback  (btaudiosrc: system_dile_startUpload(callback))
DILE_AUDIO_PCM_RegSendPCMCallback(channel, callback);
// 2. begin streaming
DILE_AUDIO_PCM_StartUpload(channel);
// ... HAL invokes callback(pcm, bytes) repeatedly on its own thread ...
// 3. stop
DILE_AUDIO_PCM_StopUpload(channel);
```

- `channel` is a small integer (a byte in `btaudiosrc`); `0` is the obvious first guess (MAIN),
  matching `DILE_AUDIO_DIRECT_CHANNEL_T { MAIN=0, SUB=1 }` in `dile_audio_direct.h`.
- `btaudiosrc` calls `RegSendPCMCallback(channel, cb)` with the callback as the **2nd** arg and
  the channel as the **1st** arg, then `StartUpload(channel)`.

## Callback prototype (the critical piece)

From `btaudiosrc_a2dp_sendStream` (the function `btaudiosrc` registers; resolved via the literal
at `0x20904` → `0x1e441`, Thumb bit = `0x1e440`):

```asm
mov r8, r0        ; arg0 -> r8
mov r7, r1        ; arg1 -> r7
...
mov.w r3, #16384  ; send() flags = MSG_NOSIGNAL (0x4000)
mov   r2, r7      ; send() len  = r7 = callback arg1
mov   r1, r8      ; send() buf  = r8 = callback arg0
blx   send
```

Therefore:

```c
// Called by the HAL with raw interleaved PCM and its byte length.
// Return 0 to continue (btaudiosrc returns 0 on the success path, 1 on error).
int send_pcm_callback(const void *pcm, int bytes);
```

`btaudiosrc` forwards the buffer straight to its A2DP audio socket (`send(fd, pcm, bytes,
MSG_NOSIGNAL)`), i.e. the HAL callback delivers **decoded playback PCM**, which is exactly what
AudioCast needs.

## What remains unknown (to determine by the first on-TV probe)

1. Exact sample format / rate / channels (S16_LE vs S32_LE; 44.1k vs 48k; stereo vs mono) — the
   HAL may deliver the media's native format, or a fixed one. A probe that counts bytes, measures
   RMS, and inspects the first samples will establish it.
2. Which `channel` value(s) are valid, and whether multiple concurrent `StartUpload` consumers are
   allowed (contention with `btaudiosrc` when Bluetooth audio-out is active, and `acr2` when
   LivePlus/ACR is enabled).
3. Whether `HAL_AUDIO_PCM_SetInfo` is needed to select format (not exported by the DILE stub on
   this build, so `RegSendPCMCallback`+`StartUpload` alone is the first thing to try).

## Safety

This HAL API is what `btaudiosrc` and `acr2` call in normal operation (they do not wedge), so it is
far safer than the broken MStar MAD ALSA capture path. Still, the first trial is run as a
standalone probe with a hard timeout and the TV is reboot-recoverable.

## On-device probe result (2026-10-21)

`tools/dile_probe.c` (ARM32, cross-compiled) was run on the real TV. Result:

- **API is callable and safe**: `RegSendPCMCallback(0)=0`, `StartUpload(0)=0`, `StopUpload` clean,
  no wedge, no leftover process, no reboot needed.
- **Callback fires at the correct rate**: 4608-byte chunks (= 1152 stereo S16 frames ≈ 24 ms) at
  ~42 chunks/s ≈ **48 kHz stereo S16** — matches the expected format exactly.
- **BUT the PCM is silence** (`rms=0`), and HAL init prints `Device or resource busy` +
  `UTOPIA ASSERT ... drvMMIO.c MDrv_MMIO_Init`.

Interpretation: the callback clock runs, but a **standalone** process that loads `libhal_msd96alvx`
initializes its own Utopia/MMIO middleware copy, which collides with the system's already-running
audio middleware (hence `Device or resource busy`), so its DMA buffer is never filled with the live
decode. `btaudiosrc`/`acr2` receive real PCM because they are launched **by the system at boot in
its audio-pipeline context** (and the system explicitly routes decoded audio to them — e.g. Bluetooth
sound-out routes to `btaudiosrc`).

### Open problem (blocking implementation)

Plain `StartUpload` from a third-party process yields silence. The remaining unknowns:

1. How a consumer gets the system to **route** decoded audio to its upload (audiod scenario/routing,
   `State::setLoopbackStatus` / `setRecordStatus` symbols, or a Luna `com.webos.audio` scenario).
2. Whether the consumer must be launched via the system service manager with the same context as
   `btaudiosrc` (pid-1 child, specific cgroup/env), rather than SSH/`nohup`.
3. Contention: only one upload consumer at a time may be allowed (BT sound-out vs ACR vs AudioCast).

This is the next reverse-engineering target. It is a routing/context problem, **not** a signature
problem (the signature is recovered above).

## Boot-context probe result (2026-10-21) — boot chime only, NOT playback

A boot-time probe (temporary Homebrew `init.d` hook, ~15s after boot, 60s capture) with
per-second RMS logging showed:

```text
progress t=0  rms_1s=0.13957   ← one loud second (the TV boot chime, ~-17 dBFS)
progress t=1..59 rms_1s=0.00000 ← pure silence for the rest
RESULT calls=2504 bytes=11538432 min=4608 max=4608 rms=0.01160
```

The single non-zero second is the TV's **startup chime**, not continuous playback. (An earlier
20s capture's `rms=0.00620` was that same chime averaged over a longer window — I initially
misread it as a breakthrough; the per-second data corrected that.) So:

- The DILE `StartUpload` consumer **does** receive audio on some system-sound path (boot chime).
- It does **not** receive media decode audio (YouTube/etc.) — media bypasses both PulseAudio and
  this upload tap (confirmed again: `pcm_output` sink stays SUSPENDED during playback).
- To capture media, the system must **route** the media decode to the consumer. That routing is
  exactly what `btaudiosrc` (BT sound-out) and `acr2` (ACR) get from the audio framework, and is
  likely controllable via `audiod`'s `setLoopbackStatus`/`loopback_set_parameters` and the Luna
  `com.webos.audio/state/set {"loopback": ...}` state.

**Honest status:** the API and callback signature are fully recovered and the probe is proven safe;
the remaining blocker is the *routing* of media decode to a third-party consumer, which is a
proprietary audiod/LS2 problem (and SSH `luna-send` is ACL-blocked, though the app's own
PalmServiceBridge context may not be).

## Additional on-device findings (2026-10-21, second pass)

- **Call order**: `btaudiosrc` calls `StartUpload(channel)` **before** `RegSendPCMCallback(channel, cb)`.
  `tools/dile_probe.c` was updated to match; the order did **not** change the silence result (still
  `rms=0` + `Device or resource busy`), so ordering is not the blocker.
- **Launch context is the key difference**: `btaudiosrc` is an Upstart job (`/etc/init/btaudiosrc.conf`,
  `start on core-boot-done`) — launched **at boot** in the system audio-pipeline context. `acr2` is
  likewise a system service. A post-boot SSH process initializes its own Utopia/MMIO middleware copy,
  which collides (`Device or resource busy` + `UTOPIA ASSERT … MDrv_MMIO_Init`) and never fills the DMA
  buffer. This is a **context/routing** problem, not a call-signature problem.
- **audiod loopback/record path** (the promising "supported" route): `audiod` contains
  `State::setLoopbackStatus`/`getLoopbackStatus`/`setRecordStatus`/`isRecording` and
  `PulseAudioMixer::loopback_set_parameters(const char*)`, plus a `precord` Pulse sink mentioned
  alongside `pcm_output`. The `com.webos.audio/state/set` JSON schema accepts
  `{"loopback": "<string>"}`. This is how media audio can likely be mirrored into a capturable sink
  **without** changing the speaker routing. Exact accepted `loopback` string values were not recovered
  (only the schema + log strings).
- **`luna-send` from SSH is empty**: every `luna-send` call (including the known-working
  `org.webosbrew.hbchannel.service/exec`, verified via a file-write side effect that did **not** run)
  returns nothing from an SSH shell. `luna-send -m <name>` DID get a hub response
  ("service name already exists"), proving the hub is reachable — so this is an LS2 client
  ACL/context issue for the SSH process, **not** a dead bus. The AudioCast **app** calls LS2 via
  PalmServiceBridge in the proper app context, so it may be able to drive `com.webos.audio/state/set`
  even though the SSH `luna-send` CLI cannot. This is untested until the app is installed on a TV.
