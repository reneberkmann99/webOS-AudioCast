# Compatibility and qualification matrix

**No physical TV model is qualified by this release.** No actual TV playback, internal speakers, HDMI, ARC/eARC, Bluetooth, optical, protected media, reboot or standby has been tested during this build. Do not convert upstream anecdotes, an open device, or synthetic integration tests into a supported-model claim.

| Model / environment | SoC / architecture | Firmware / version | Source/backend | Format | Evidence/status |
|---|---|---|---|---|---|
| Upstream reference TV (model unspecified) | Realtek / MARS capture label | webOS7.4.0 | `hw:0,13` MARS CAPTURE_TEST | S16_LE48k2ch | **Externally reported only**, not tested with AudioCast; source/output-specific details unknown |
| Developer Linux host | x86_64 | Debian Linux | Generated tone, real sockets | S16_LE; S32_LEHTTP; mono/stereo tests | Core implementation tested, **not a TV compatibility entry** |
| ALSA ABI test library | x86_64 | Test fixture | Partial reads, silence, xrun, ENODEV, wrong rate, suspend/EAGAIN | Explicit test buffers | Backend failure handling tested, **not real ALSA/hardware playback validation** |
| QEMU ARM application | ARMv7 soft-float / Debian ARM libc | Emulated userspace | Native executable + generated test source | RTP/HTTP tested separately | Architecture/core smoke/integration only, **not LG loader/driver validation** |
| Any 64-bit-only TV without32-bit runtime | aarch64 only | Any | Current distributed32-bit binary | — | Unsupported by this binary; no bridge automatically installed |
| Other Realtek / LG / other SoC models | Unknown | Any | Device inventory/manual candidates | Unknown | **Untested**, not assumed to expose a tap |
| PulseAudio monitor | Firmware-specific | Any | Not implemented | — | Not supported in0.1; server/source inventory only |
| Proprietary vendor capture / Opus / multichannel>2 / encoded passthrough | Unknown | Any | Not implemented | — | Not supported in0.1 |

## Required real-TV test worksheet

For each report, record exact model, SoC evidence, architecture, firmware/webOS/kernel, root/Homebrew version, card/device name+index, capabilities and selected format. Record actual source/output behavior independently:

| Source | Internal speakers | Bluetooth | Optical | ARC/eARC | Result / observed caveats |
|---|---|---|---|---|---|
| Authorized local/internal application | Untested | Untested | Untested | Untested | |
| HDMI input authorized PCM content | Untested | Untested | Untested | Untested | |
| Broadcast/live tuner (authorized) | Untested | Untested | Untested | Untested | |
| Source switching mid-stream | Untested | Untested | Untested | Untested | |
| Mute/unmute | Untested | Untested | Untested | Untested | |
| User volume changes | Untested | Untested | Untested | Untested | |
| Standby/wake, reboot/autostart | Untested | Untested | Untested | Untested | |

Use distinct left/right authorized test tones and silent sections. Verify captured sample provenance by listening/comparing a requested short receiver diagnostic, not meter movement alone. Confirm speakers/output continue normally and this application did not modify routing or gain. Observe clipping, channel order, native rate and PCM vs compressed passthrough. Do not manipulate DRM or infer protected-source support from unrelated unprotected media.

Use `tested`, `not tested`, `unsupported on this combination`, or `partial` per cell. Do not mark the whole TV supported because one app works. If a driver opens but returns silence while known content plays, record **no demonstrated playback capture**, not success. If no verified tap exists, stop here and report the limitation; never substitute microphone input or change TV audio routing to make a demo appear to work.

Before stable/public central-repository submission: human installation/capture review, source/output matrix, reboot/uninstall/rollback, physical remote usability, multi-hour resource/performance test and security/license review are mandatory. See `docs/publication.md`.
