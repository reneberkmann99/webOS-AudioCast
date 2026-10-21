# Protocols and API

## RTP/L16 (primary)

- RTP v2, dynamic payload type **96**, RFC3550 header / RFC3551 L16 payload.
- PCM signed16-bit **big-endian/network order**, interleaved channel order preserved. Stereo channels are original selected capture order, not remixed. HTTP uses little-endian instead.
- Timestamp clock is the selected sample rate, incremented by **frames per channel**, not bytes or total samples. Sequence, SSRC and initial timestamp use OS randomness. Sequence/timestamp wrap naturally.
- Requested5/10/20ms logical capture blocks are split into RTP datagrams of at most1200bytes (12byte header, <=1188byte frame-aligned PCM). At48k stereo a10ms block is297+183frames;20ms requires4datagrams. Smaller final fragments have corresponding timestamp increments. There is no fixed20ms wire packet assumption and no misleading fixed SDP ptime.
- Marker on initial/discontinuous block; no marker on every fragment. Ring overruns/device recovery mark discontinuity. Dropped queued blocks preserve sample counter gaps.
- Nonblocking unicast UDP. Public/multicast/broadcast destinations rejected. Standard RTP is unencrypted/unauthenticated; explicit user start on a trusted LAN is required. HTTP tokens do not authenticate UDP.
- RTCP compound sender-report + SDES CNAME sent initially and at randomized~2.5–7.5second intervals; compound BYE on orderly RTP teardown. RTCP normally uses destination RTPport+1 (65534 when RTPport65535), explicitly advertised with SDP `a=rtcp`.
- Sender report NTP/RTP mapping is anchored to **capture-read completion/sample counter**, not a calibrated TV playback/capture-point clock. Driver pending audio and system clock error are not eliminated. It must not be presented as measured end-to-end latency. Receiver reports/RTT estimation, multicast session membership, retransmission, RTSP negotiation and SRTP are not implemented.

Format must be supplied to the receiver or obtained from the authenticated `/stream.sdp`. Default:

```sdp
v=0
o=- 0 0 IN IP4 127.0.0.1
s=webOS AudioCast
c=IN IP4 192.168.1.20
t=0 0
m=audio 5004 RTP/AVP 96
a=rtpmap:96 L16/48000/2
a=rtcp:5005
a=sendonly
```

The origin field is an identifier, not the TV's network source address. For interoperable players, save actual generated SDP (adjust any receiver-specific destination binding as needed):

```sh
# On a trusted computer; token entered separately, not embedded in URL.
curl -H "Authorization: Bearer $(cat stream-token.txt)" http://TV_IP:7070/stream.sdp > audiocast.sdp
ffplay -protocol_whitelist file,udp,rtp -i audiocast.sdp
```

ffplay interoperability is an on-device/manual test instruction, **not a claimed executed test**. The reference Python receiver is covered by automated socket integration tests. Opus/RTP is not implemented.

## HTTP audio (7070)

Loopback-only by default; explicit `http_lan:true` binds IPv4 interfaces. Every endpoint requires `Authorization: Bearer STREAM_TOKEN`. Distinct from control token. No token query parameters, cookies, URL credentials, CORS-based unauthenticated access, or network control endpoints.

| GET endpoint | Response |
|---|---|
| `/audio` | HTTP/1.1 chunked `application/octet-stream`; native interleaved S16_LE or S32_LE PCM |
| `/metadata` | JSON `sample_format`, `rate`, `channels`, `interleaved`, `byte_order`, packetization metadata |
| `/stream.sdp` | `application/sdp`; RTP/L16 format/destination information |

`/audio` also sends `X-Audio-Rate`, `X-Audio-Channels`, `X-Audio-Format`. HTTP clients must retain incomplete sample-frame bytes across arbitrary HTTP/TCP/read boundaries: HTTP chunks are not audio frames. No WAV header with a fake finite length is used. The raw endpoint is deliberately explicit; use the reference receiver's opt-in WAV writer for a finite diagnostic capture.

Only active HTTP/Both streaming accepts audio clients; otherwise409. Metadata remains accessible idle. Maximum8pending/active clients. Application queues<=32KiB each; slow consumers disconnect independently, never stall capture. Stop/config-generation changes close active streams; clients must reconnect. Receiver disappearance, framing/auth errors and socket sends are handled without unlimited queues. No built-in HTTP TLS; trusted LAN/VPN only.

## Control (127.0.0.1:7071 ONLY)

All ordinary methods require `Authorization: Bearer CONTROL_TOKEN`. CORS preflight allows the local file-origin TV app (`Origin:null`) but does not waive authentication. Stream tokens cannot control the daemon. Control is not offered on LAN. Max16KiBrequest total/8KiBbody, duplicate headers and chunked request bodies rejected, single request/connection.

| Method/path | Behavior |
|---|---|
| GET `/v1/status` | Real capture state, signal, levels, requested streaming, format, source confirmation, errors, HTTP client count, packet/drop counts, bytes/sec, sampled CPU/RSS, read-to-sender queue age, ALSA pending frames/time; total latency/RTP receiver count unknown |
| GET `/v1/devices` | Inventory + CAPTURE_TEST hints only, no microphone scan/open; backend availability |
| GET/POST `/v1/config` | Fetch/validate+persist partial config; changes only idle/resource-released; unknown fields rejected |
| POST `/v1/start` | Explicit request, requires explicit source and playback confirmation; inspect later real state |
| POST `/v1/stop` | Stop intent, release capture, drop queues/close audio clients |
| POST `/v1/test-capture` |5second actual local PCM test; meters only, never network/audio storage; expiry also on capture failure |
| POST `/v1/test-connection` | LAN resolution/socket feasibility or HTTP listener guidance; `reachable:null`, `delivery_verified:false` because no receiver handshake |
| POST `/v1/suspend`, `/v1/resume` | Manual release/reopen preserving prior stream intent |
| GET `/v1/logs` | Last150bounded event lines, no tokens/audio |
| GET `/v1/diagnostics` | Sanitized system/inventory/config/status; no credentials/audio |

API errors return JSON `error` with HTTP400 currently (including busy/unknown endpoint validation); authentication401. Accepted commands normally200with explicit fields, not proof of valid source/network delivery. Health can be checked with authenticated status. A starting/recovering engine is not reported as successfully capturing.

Default config is defined by `daemon/common.hpp` and initialized as JSON in the protected state directory: ALSA explicit empty device, S16_LE48000stereo, RTP127.0.0.1:5004,10ms, low-latency, LAN HTTPoff, boot streamingoff, playback confirmationoff. CLI `--allow-test-source` is test-only and never passed by installed helpers.
