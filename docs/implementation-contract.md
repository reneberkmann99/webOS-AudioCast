# Implementation contract (0.1.0)

This is a hardware-unqualified engineering prerelease. No TV capture is claimed until on-device tests.

## Native process

`audiocastd --state-dir PATH [--allow-test-source]` runs idle initially; state defaults to `/var/lib/webos-audiocast`. `--version`; `--self-test` for protocol tests. Signal TERM performs clean shutdown. Lock state directory against duplicate instances.

Root supervisor is launched via Homebrew Channel exec of a fixed installed `service/startup/manage.sh ensure`. UI is unprivileged. Bootstrap prints JSON `{control_token,stream_token,control_url:"http://127.0.0.1:7071",returnValue:true}`; tokens are random, persisted mode 0600, never in URLs. Control API loopback only, Bearer control_token, CORS permits `null`/file origins with token. LAN stream listener default disabled (bind loopback unless http_lan true). Separate stream bearer token for read-only audio/metadata. LAN control is not offered in 0.1.

## REST control API (7071, authenticated JSON)

- GET `/v1/status`: `state` idle/starting/capturing/recovering/paused/error; `streaming` boolean; `backend`, `device`, `source_verified`, `signal` unknown/silence/detected; `format:{sample_format,rate,channels}`; `audio_output` string or "unknown"; `destination`; `transport`; `http_clients`; `rtp_packets`,`rtp_errors`,`queue_drops`,`capture_overruns`; `throughput_bps`; `cpu_percent`; `rss_kb`; `levels:{rms:[0,0],peak:[0,0]}` (linear 0..1); `capture_to_network_ms` null or measured queue age; `end_to_end_ms:null`; `last_error`; `uptime_seconds`; `config` (no tokens); `test_active` boolean; `silence_seconds`.
- GET `/v1/devices`: `{backends:[...],devices:[{id,name,backend:"alsa",candidate:bool,verified:false}],warning}`. Inventory only; no probing arbitrary microphones automatically. MARS CAPTURE_TEST is a candidate, NOT validated just by name. PulseAudio monitor backend is marked unavailable unless actually implemented; never use default/mic source.
- GET `/v1/config`: config JSON.
- POST `/v1/config`: JSON config or partial changes, returns validated merged config; changes only while idle. Config fields `backend:"alsa"`, `device:""`, `sample_format:"S16_LE"` (S32_LE supported HTTP only), `rate:48000`, `channels:2`, `transport:"rtp"|"http"|"both"`, `destination:"127.0.0.1"`, `destination_port:5004`, `packet_ms:10`, `profile:"low-latency"|"stable"`, `http_lan:false`, `autostart:false`, `playback_confirmed:false`. Backend `test-tone` exists ONLY with CLI --allow-test-source, never package startup. Device or format changes invalidate playback confirmation unless explicitly re-confirmed. UDP data is unencrypted; explicit start authorizes transmission.
- POST `/v1/start`: require device and playback_confirmed; explicit user start. Start returns accepted; status shows actual hardware readiness.
- POST `/v1/stop`: stops capture and streams.
- POST `/v1/test-capture`: read selected device for 5 seconds, no network output; returns accepted; real meters/status; no file storage. Opening/nonzero does NOT establish TV playback provenance; user must correlate to known authorized TV playback and confirm selected source.
- POST `/v1/test-connection`: bounded UDP send/resolve on worker/control thread; result `{reachable:null,delivery_verified:false,message:...}`. UDP send is NOT proof of delivery. HTTP mode returns listener state. Never ping via shell.
- GET `/v1/logs`: `{lines:[...]}`, bounded ring (tokens never logged).
- GET `/v1/diagnostics`: sanitized JSON `{status,devices,config,system}`, no tokens or captured audio. Shell diagnostic report additionally available via scripts/diagnose.sh.
- POST `/v1/suspend` and `/v1/resume`: release capture device and restore prior streaming intent; platform power subscription is best effort, hardware errors back off. UI can operate these too.

API errors JSON `{error:"..."}` HTTP 400/409/401. Limited request bodies; reject unknown config fields.

## Network data

- RTP v2, dynamic payload type 96 L16, NETWORK big-endian signed 16-bit, sample clock timestamp (increments by frames), random initial seq/SSRC/timestamp. SDP `a=rtpmap:96 L16/<rate>/<channels>` available.
- 10/20ms logical capture blocks are split into <=1200-byte RTP datagrams to avoid IP fragmentation. Stereo48k max297 frames/packet (1188 payload+12 header). Timestamp increments by actual frames per datagram. Marker on initial/discontinuous block.
- HTTP7070 GET `/audio`: `application/octet-stream`, chunked S16_LE or S32_LE interleaved PCM; headers `X-Audio-Rate`, `X-Audio-Channels`, `X-Audio-Format`; exact raw format also GET `/metadata` JSON and `/stream.sdp`. No ambiguous endless WAV. All require `Authorization: Bearer STREAM_TOKEN`. At most8 concurrent clients, bounded queues, slow consumers disconnected, no recording. No control routes on this listener.
- RTP listener count unknown (null), not inferred from sends. HTTP count is measured.

Reference receiver CLI: `python3 receiver/receiver.py rtp --bind 0.0.0.0 --port 5004 --rate 48000 --channels 2` or `http --url http://TV_IP:7070/audio --token-file FILE`; `--play` optional sounddevice dependency; `--record PATH --duration 10` explicit recording; no saved audio by default; packet loss/out-of-order/jitter stats; bounded buffers; Python callable processing hook; UDP probe cannot prove delivery; end-to-end latency unknown without external synchronized test.
