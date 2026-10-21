# Audio architecture research and feasibility

Research date: **2026-10-21**. This is a source-backed feasibility investigation, not a hardware qualification report. The available TV address did not accept SSH; no TV was powered on, rooted, reconfigured, or installed to during this development session.

## Evidence inspected

| Source | Revision inspected | Findings / license |
|---|---|---|
| [webos-audiostream-remote](https://github.com/nickvsnetworking/webos-audiostream-remote) | `e5504ed52281b1bf52d450f283d12b1e3211863c` | README reports Realtek MARS CAPTURE_TEST, `hw:0,13`, S16_LE/48k/stereo, webOS7.4.0, capture independent of output routing. No license declared. **No code reused.** TV model/source-by-source evidence and measurements are not supplied there. |
| [Homebrew Channel](https://github.com/webosbrew/webos-homebrew-channel) | `1a2b969e992617c9ba4b8a891542cb93a9e07b0a` | MIT. README documents root `exec`/`spawn`; startup.sh invokes `run-parts /var/lib/webosbrew/init.d`; root is not guaranteed merely because exec succeeds. |
| [PicCap](https://github.com/TBSniller/piccap) | `a243d0e0f4426f83e270553c4a5631964ee74358` | MIT. Frontend + separately persistent privileged native capture is an appropriate architectural pattern. Video capture backends/DRM behavior do **not** establish equivalent audio availability. |
| [hyperion-webos](https://github.com/webosbrew/hyperion-webos) | `00c932f092084b234b3f619b060a98fa134c12bd` | MIT. Multiple model-dependent video capture backends illustrate why universal hardware assumptions are unsafe. No audio backend copied. |
| [apps-repo](https://github.com/webosbrew/apps-repo) | `291da2bce269fbc901a0716b645caf64f709e3dc` | Apache-2.0. Current README explicitly rejects primarily AI-generated applications without meaningful human development/testing/review; AI assistance must be disclosed. Manifest and YAML schemas examined. |
| [webOSbrew development resources](https://www.webosbrew.org/develop/) | fetched 2026-10-21 | Recommends ares-cli-rs, native-toolchain, Dev Manager; publishing and native/web-app guides. |
| [webos-userland headers](https://github.com/webosbrew/webos-userland/tree/main/include) | inspected 2026-10-21 | `dile_audio_direct.h` describes decoder/mixer playback paths and Write/Start/Stop; `lgnc_directaudio.h` exposes Open/Play/Close. These are **playback injection**, not system playback capture. No vendor headers copied. |

Raw downloaded third-party source/headers are excluded from releases. Publicly accessible code without a license is not permission to redistribute it.

## What actually follows from this evidence

LG TV media can pass through application decoders/GStreamer, vendor decoder/mixer and DSP/HAL paths, and hardware output routing. Internal apps and HDMI/broadcast sources need not traverse the same Linux userspace PCM stream. The inspected direct-audio APIs distinguish decoder and mixer resources; they do not expose a verified readback API. A generic Linux `default` recording device is therefore not a system-audio monitor.

On the **specific upstream-reported Realtek firmware**, a capture PCM named MARS CAPTURE_TEST is reported to tap the internal playback bus before output selection. This is a plausible non-routing capture path. Its exact location relative to volume/mute, DSP/AI audio processing, codec passthrough, and protected paths remains **unverified here**. We cannot extrapolate from that report to other Realtek revisions, LG-designed SoCs, MediaTek-derived platforms, or later firmware.

ALSA card/device numbers are enumeration indices, not stable API identifiers. AudioCast inventories `/proc/asound/pcm`, annotates CAPTURE_TEST names as **candidates only**, and accepts explicit `hw:CARD,DEVICE`. There is no hardcoded production `hw:0,13` default, no `default`/mic fallback, no ALSA loopback module installation, and no mixer mutation.

## Sound servers and vendor services

The presence of PulseAudio libraries, a `pactl` executable, or a webOS OSE audio API does not prove TV media is mixed through a PulseAudio sink. Diagnostics query an actual server's source inventory if available, but **PulseAudio capture is not implemented in 0.1.0**. A future backend must select a real `.monitor` source and prove it carries TV playback; it must never select a default recording input. OSE documentation is not treated as commercial TV firmware compatibility evidence.

The read-only `com.webos.service.audio/getSoundOutput` query is best effort and firmware dependent. The supervisor tries it with a timeout; only a successful response containing `soundOutput` becomes dashboard telemetry, otherwise `unknown` is shown. No `setSoundOutput`, volume, mute, settings-write, Bluetooth/ARC routing, or protected-media API is called. On-device behavior/permissions of this telemetry query are unverified.

## Outputs, sources, and protection

- Upstream reports independence from TV speaker/Bluetooth/optical/ARC output selection on one configuration. AudioCast makes no blanket support claim for these routes.
- HDMI audio can reach vendor hardware paths without appearing in any ordinary Linux capture device. A successful ALSA open while HDMI is selected is **not proof** HDMI playback was captured.
- Compressed passthrough/eARC and surround decoding may bypass a PCM tap, change rate/channel layout, or yield silence. 0.1 supports explicit mono/stereo native S16_LE; S32_LE is HTTP-only. No covert decoding or resampling occurs.
- DRM/protected playback may intentionally yield silence or deny capture. Do not bypass this. PicCap's video restrictions are not evidence about audio permission or access.
- Silence is a real possible signal state (mute, quiet content, wrong tap, or protection). Nonzero samples are also not enough: a microphone or unrelated hardware bus could be nonzero. Human correlation with a known, authorized playback sample is mandatory.

## Selected implementation

A C++11 native process uses runtime `libasound.so.2` with nonblocking capture. Only the explicit capture PCM is opened; no playback/mixer handle is created. A bounded queue feeds nonblocking UDP RTP/L16 and authenticated HTTP raw PCM. An unprivileged ES5 TV web app bootstraps the detached root supervisor through a fixed Homebrew exec command, then controls an authenticated loopback API. This avoids Node.js in the capture path and avoids needing a new privileged LS2 service/third-party runtime on the TV.

A backend interface exists; ALSA is the only TV capture backend currently implemented. A generated test-tone backend is explicitly gated by a CLI test flag and never selected by the TV helper/UI. No verified accessible capture path on a TV means **unsupported until demonstrated**, not permission to route/record microphone audio.

## Build-tool qualification

The official pinned 2026.08-webos SDK cross-compiles the ARM binary. Its bundled runtime libraries failed even a trivial hello-world under this host's QEMU. A Debian ARM libc runtime successfully ran both the hello-world and AudioCast ARM self/integration tests. Thus QEMU evidence validates executable architecture/application behavior against that runtime, **not the LG firmware loader/libc**. A real TV loader/ALSA/API test is still a release gate. No proprietary runtime libraries are bundled to hide this uncertainty.
