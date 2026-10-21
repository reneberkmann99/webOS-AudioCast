# Architecture

## Process and privilege boundaries

```text
Unprivileged TV web app (ES5, remote navigation)
   | fixed Homebrew Channel exec: manage.sh ensure / enable / disable / diagnose
   | credentials returned privately through local Luna reply; never URLs/storage
   v
Root supervisor (detached, lock/identity guards, bounded crash backoff)
   | starts / watches / stops on app removal
   v
Native audiocastd (root only because vendor capture permissions may require it)
   |-- main thread: loopback7071 authenticated finite REST control
   |-- capture thread: explicit ALSA PCM, no playback/mixer writes
   |-- network thread: bounded ring, nonblocking RTP/RTCP and HTTP7070
   |-- config + credentials: /var/lib/webos-audiocast (0700; files0600)
   `-- bounded in-memory event log (150 lines); no TV audio files

TV playback --> supported hardware bus capture --> native PCM read
                           TV normal output path remains unmodified by our code
PCM --> RMS/peak --> 8-block drop-oldest ring --> RTP/L16 or HTTP raw PCM
                         | slow network never blocks the capture thread
                         ` HTTP: <=8 clients, <=32KiB application queue per client
```

No separate JS/Node Luna service is required. The real Luna integration is `app/src/hbchannel.js`; privileged lifecycle scripts are in `service/startup/`. Packaging places both under the app so uninstall removes all executable startup targets. A symlink, not a copied executable, is installed in Homebrew init.d.

The root process is intentionally small. It does not expose shell execution, file upload/path APIs, arbitrary device plugins, arbitrary ALSA plugin names, output routing, or volume control. GUI and reference receiver do not need root. Further privilege dropping cannot be assumed safe across vendor device ownership/reopen requirements; sandboxing/drop-to-service-user is a future TV-qualified improvement, not a claimed feature.

## Capture

- ALSA dynamically resolved from `libasound.so.2`; missing library/symbols produce explicit errors.
- Explicit `hw:CARD,DEVICE`, CAPTURE, NONBLOCK, RW_INTERLEAVED. Mono/stereo, selected native S16_LE/S32_LE rate. Native rate negotiation must match exactly; no hidden resampler or gain.
- Period requested from 5/10/20ms block size; driver may negotiate a nearby period/buffer. Stable requests8 periods, low-latency3. Hardware pending frames are reported separately; true hardware capture-point latency is unknown.
- Partial frame reads are kept intact. Overruns prepare/restart with discontinuity markers. Suspend gets one resume attempt; EAGAIN closes/releases and backs off rather than using ALSA's potentially unbounded suspend-recovery loop.
- Empty reads timeout after3seconds. Device failures reopen with exponential100ms..10s backoff, interruptible on stop. A 5second capture-only test stops even when opening repeatedly fails.
- Generated test tone requires `--allow-test-source`; production helpers never pass this, GUI never offers it. ALSA ABI doubles exist only under tests/build directories and are never packaged.
- Device/format/backend changes invalidate prior confirmation unless explicitly re-confirmed. Configuration changes require idle/resource-released state. Opening/nonzero alone never verifies provenance.

## Buffer and network ownership

The capture thread never resolves hosts or sends sockets. Its drop-oldest queue has8 blocks, with frame counters preserving RTP discontinuities. The network thread rejects queued blocks older than100ms. Each HTTP client has a32KiB application queue and limited kernel send buffer; excess queue disconnects only that client. UDP uses nonblocking send and counts errors/drops instead of retrying stale PCM. All allocations and request/client counts are bounded by validated formats, max96k stereo32-bit, block<=20ms, and fixed client limits.

DNS resolution occurs only off the capture thread (network/control operations). OS `getaddrinfo` timeout behavior is platform-dependent and is not given a fabricated hard bound. Numeric LAN IP destinations avoid DNS stalls and are recommended for unattended low-latency operation. Capture continues dropping old network queue entries if DNS is slow. The supervisor enforces a5second grace before kill during an explicit whole-service stop.

## Lifecycle

First installation is idle. Press Start is explicit authorization to stream; enabling boot streaming is a separate explicit consent. Start acceptance is not proof of an open/valid capture source: inspect actual state/signal/receiver.

Closing the GUI does not send Stop. `nohup` plus `setsid` where available detaches the supervisor. Process PID checks validate exact `/proc/PID/cmdline` arguments before signals; native flock prevents duplicate daemons. Supervisor mkdir/owner locks prevent duplicate supervisors; a lock missing its owner is not blindly removed during a concurrent startup. The supervisor detects removed app binary and stops the child. Crashes restart with1..30s backoff; eight consecutive short failures quarantine until explicit Reconnect. Fatal startup stderr is rotated; supervisor log is size-bounded; engine event logs are in memory.

`/v1/suspend` releases capture and preserves streaming intent; `/v1/resume` restores it. **Automatic TV standby event subscription is not implemented/qualified.** Hardware errors are recovered; a tap that remains open during standby cannot be assumed to trigger that fallback. Use manual release or stop until this TV's power integration is qualified. Do not claim multi-hour unattended/standby qualification from host lifecycle tests.

## Security and privacy

Control binds only127.0.0.1:7071, requiring a separate random control bearer. Streaming7070 is loopback-only by default; explicit LAN enable binds IPv4 interfaces and requires the distinct stream bearer for audio, metadata, SDP. It has no control routes. No wildcard CORS is offered; local file-origin GUI access requires the control bearer. Request bodies/headers are capped, ambiguous duplicate/chunked request framing is rejected, and HTTP clients cannot build unlimited queues.

RTP is intentionally unencrypted/unauthenticated standard PCM RTP on a trusted LAN. DNS results must be private/link-local/loopback unicast; public targets/multicast/broadcast are rejected. HTTP bearer credentials/audio are cleartext on LAN; use trusted isolated networks or an external VPN. No internet exposure, UPnP, port forwarding, or firewall edits. Secrets are never logged, included in diagnostics, or inserted into stream URLs. See `docs/security.md`.
