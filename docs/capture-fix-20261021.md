# MStar playback capture — actual-TV acceptance, 2026-10-21

## Outcome and boundary

**YouTube playback capture now works on the inspected MStar TV through the installed native engine.** This is not an audible receiver or continuous-production qualification. The owner chose capture-only verification; experimental capture is limited to 30 seconds per manual start. Streaming autostart remains disabled; background-service boot startup remains enabled.

No microphone/default fallback, MAD ALSA opens, DRM/firmware/permission changes, speaker-volume or routing setters, or TV-audio files were used for this fix. Receiver destination/settings and credentials were preserved. Source/gain consent were intentionally saved for the explicitly approved bounded DILE tests.

## Root cause and recovered initialization

The original upload gain was `12+0`. Static analysis of this firmware's `acr2::AudioCapture::StartCapture()` revealed the omitted `DILE_AUDIO_PCM_SetVolume(0x007f)` call. The wrapper controls the PCM-upload DSP gain, not the ordinary speaker-volume API. On this build the original gain's conversion saturated upload attenuation.

The approved experiments used `12+0 → 127+0 → 12+0`, verifying restoration after `StopUpload`. Utopia/MMIO initialization warnings did **not** establish a fatal launch-context or routing problem: a standalone SSH process and the installed daemon both delivered real playback-correlated PCM once upload gain was initialized.

## Playback provenance, controlled by the owner

Each phase was five seconds / 208 callbacks / 958464 bytes:

| Phase | RMS | Peak |
| --- | ---: | ---: |
| YouTube playing | 0.148537 | 0.313568 |
| Owner paused YouTube | 0.000010 | 0.000061 |
| Owner resumed YouTube | 0.135366 | 0.316101 |

Evidence: `build/dile-playback-correlation-proof.json` and its referenced logs. This distinguishes media capture from noise or a startup chime; it does not establish every other source/output mode.

## Integrated native backend on the TV

The initial mutex handoff dropped buffers on the loaded TV. Replacing it with eight preallocated chunk slots removed reader/callback lock contention. The final direct native-backend probe measured:

- 186 callbacks / 857088 callback bytes; **0 dropped bytes / 0 discontinuities**.
- 213600 frames read; RMS `[0.1445577, 0.1606465]`, peaks `[0.3121948, 0.3131714]`.
- Total 4.9876 seconds, including 0.5132 seconds opening/initialization.
- Gain restoration verified; recovery record absent; no residual probe process.
- No captured audio saved or sent.

Evidence: `build/dile-native-tv-chunk-queue.log`.

The installed daemon was then tested through its authenticated API, not just a research executable:

- Explicit `backend=dile`, `device=mstar-upload:0`, 48 kHz/stereo/S16_LE.
- `/v1/test-capture` produced stereo RMS maxima `[0.1744710, 0.1982210]` over 16 status observations.
- **0 capture-overrun/discontinuity counter increments** and no RTP packet-counter increments during that capture-only test.
- End state idle, capture resources released, gain restoration verified, no pending recovery, autostart false.
- The optional post-capture callback-byte summary is absent when the watchdog-expiry path closes capture; the independent probe supplies the exact byte-drop count above. These are distinct measurements.

Evidence: `build/proofs/tv/dile-api-capture.json` and `build/dile-api-tv-accept.log`.

A later explicit stream-start event while the GUI was open ran until its 30-second grant ended and automatically stopped with gain restoration verified. Its destination was the unchanged TV-loopback `127.0.0.1:5004`; this is **not** an audible external-receiver test. Subsequent GUI form/configuration edits were left untouched. The final read-only audit found the saved source cleared (`backend=alsa`, `device=""`), autostart false, idle state and gain restoration verified. Choose `mstar-upload:0` with the consent checkboxes to use the newly installed opt-in path; no attempt was made to overwrite those later edits.

## GUI, installation and safety behavior

- Installed IPK: `org.webosbrew.audiocast_0.1.1_arm.ipk`, 359862 bytes.
- SHA-256: `d022e5e3266b82a87105606a8640b2d7982c7e2223d5ad1301a95fe80c338ef0`.
- The actual Chrome 53 TV GUI is styled, service-connected, boot-enabled/manual-streaming, and offers `mstar-upload:0` with separate gain consent and a visible 30-second limit. Unsafe MAD devices remain disabled. Destination fields are visible; no horizontal overflow.
- No automatic source selection or DILE autostart. Device/format/consent validation is enforced natively, not only in the GUI.
- Exact inspected library sizes/FNV fingerprints plus loaded symbol offset gate the experimental HAL path. These are compatibility guards, not cryptographic code authentication.
- A private gain-transaction record precedes the setter. Normal stop/errors restore and verify gain; an independent timer ends the grant. Failures never retry/reopen automatically or fall back to ALSA. Suspend ends the grant; resume cannot extend it.
- Supervisor recovery runs only after the recorded owner exits, is externally bounded, and quarantines on failure. A wedged HAL/kernel or power loss can still require manual recovery/reboot; this is not a production guarantee.
- Opening the foreground AudioCast app can pause the media app. Near-zero meters in that situation are not proof that the playback tap failed. Keep the playback app active for media capture.

Private pre-upgrade rollback: `/tmp/audiocast-pre-dile-20261021-234741.tar.gz`; original config: `/var/lib/webos-audiocast/config.pre-dile-20261021-234741.json`. Credentials were not changed or exported into the repository.

## Regression evidence

- **56 host tests** pass, including vendor-ABI PCM delivery/order through real host RTP sockets, bounded queue overload, explicit test-fixture gating, consent/format rejection, gain/registration/start failures, malformed PCM, total deadlines, exclusivity, late callbacks, owner-only rollback, supervisor recovery/quarantine, and existing ALSA/receiver/service regressions.
- **17 real-native-API browser checks** plus **11 frontend-only DILE control checks** pass; zero browser errors. Host PCM/Homebrew/vendor doubles are explicit and do not establish TV behavior.
- ARM build and real-TV execution/self-test pass. Actual GUI proof: `build/proofs/tv-dile/`; synthetic screenshots are separately named fixtures.

## Deliberately unfinished

External/audible receiver delivery, unlimited capture, long-duration soak/drift/latency, HDMI/tuner/other app sources, Bluetooth/optical/ARC/eARC combinations, standby/wake behavior and general firmware compatibility remain unqualified. Do not remove the test cap or enable unattended DILE capture from these results alone.
