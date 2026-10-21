# Reference receiver

Python>=3.10 on Linux/macOS/Windows. Receiving, levels, stats, processing hooks, finite WAV capture use only the standard library. Windows/macOS execution and actual speaker playback have **not** been tested here; Linux socket/recording tests have. Optional playback uses sounddevice/PortAudio; install separately, not on the TV.

## RTP

```sh
python3 receiver/receiver.py rtp --bind 0.0.0.0 --port 5004 --rate 48000 --channels 2
# Pin expected TV source IP and reduce jitter buffer after measuring a stable LAN:
python3 receiver/receiver.py rtp --port 5004 --source TV_IP --jitter-ms 20
# Optional computer playback (not a change to TV routing/volume):
python3 -m pip install -r receiver/requirements.txt
python3 receiver/receiver.py rtp --port 5004 --play
```

Linux playback may need the distribution's PortAudio library (`libportaudio2` on Debian). macOS/Windows pip wheels and device selection depend on your host audio stack. Start playback cautiously on your own receiver; malformed/unverified hardware data should first be inspected with levels/sample diagnostics, not assumed safe PCM.

The receiver validates RTPv2, payload type, CSRCs/extensions/padding and frame alignment. It accepts the configured L16 rate/channels, converts network byte order, handles16-bitsequence/32-bittimestamp wrap, duplicate/reordered packets, SSRC changes and discontinuity markers. It pins the first sender's IP (or `--source`), but allows a changed sender UDP source port on restart. This is **not authentication**; use a trusted LAN.

A timestamp-indexed buffer has a256packet /500ms horizon hard bound, starts after configured jitter delay (default30ms), and plays out10ms blocks. Missing samples become silence with explicit missing-frame/underrun counts; late data is discarded. Long local consumer suspension clears stale backlog instead of replaying seconds of old audio. A source switch/restart resets the buffer on new SSRC/discontinuity.

Sample clock drift between TV and receiver is not silently corrected by resampling. Sustained drift can cause dropped/inserted silence frames/underflows; inspect counts and qualify long sessions. Adaptive resampling/clock discipline is a future feature. This matters for multi-hour playback; visualization hooks can consume clocked samples without an independent soundcard clock, but no universal drift-free guarantee is made.

JSON status includes actual RMS/peak, unrepaired sequence gaps (can decrease on reordering), duplicates/reordered/late/overflow counts, missing playout frames, interarrival jitter and optional playback underruns. Receiver jitter target is a setting, **not measured TV latency**. `end_to_end_ms` is null. RTCP receiver reports and synchronized-latency calculation are not implemented.

## HTTP

On TV select HTTP/Both, enable LAN explicitly, and obtain the stream-only bearer in Configuration. Store only the token in a protected local text file:

```sh
python3 receiver/receiver.py http --url http://TV_IP:7070/audio --token-file stream-token.txt
python3 receiver/receiver.py http --url http://TV_IP:7070/audio --token-file stream-token.txt --play
```

Rate/channels/sample format are read from real stream headers. Both S16_LE and S32_LE work; incomplete interleaved frames are reassembled across arbitrary network/HTTP boundaries. HTTP redirect following and ambient proxies are disabled to prevent bearer forwarding to another host. Read/socket queues are bounded. A slow consumer or corrupt/truncated stream fails visibly rather than discarding arbitrary bytes and misaligning samples. HTTP disconnect is detected; this reference CLI currently requires relaunch/reconnect after a broken TCP stream or explicit TV Stop/format change. Automatic bounded HTTP client reconnect is not claimed.

## Explicit diagnostic recording

Nothing is stored unless `--record` is supplied, and recording also requires an explicit finite duration<=3600seconds. Existing paths are never overwritten. Files are normal finite WAV with actual sample count on close. Tests use generated samples in temporary directories only, never actual TV media.

```sh
python3 receiver/receiver.py rtp --port 5004 --record authorized-test.wav --duration 10
python3 receiver/receiver.py http --url http://TV_IP:7070/audio --token-file stream-token.txt --record authorized-test.wav --duration 10
```

Audio-processing/playback/recording work runs on a separate bounded worker queue (8blocks,10ms each). If slow, oldest whole frames are dropped and `sink_drops` increments. A blocking user hook can therefore lose samples; it cannot create unbounded memory or block the networking owner. Hooks must return promptly; a stuck worker is reported on shutdown.

## Lighting / FFT integration

A trusted local hook receives native little-endian PCM with metadata; there is no network-delivered code execution:

```python
# custom_lighting.py, importable on the receiver's Python path

def process(pcm: bytes, metadata: dict):
    # metadata: rate, channels, sample_format. Samples interleaved S16_LE/S32_LE.
    # Forward into your bounded FFT/beat/LED engine queue here; do not block.
    lighting_queue.put_nowait((pcm, metadata))
```

```sh
# When the hook is next to receiver.py:
python3 receiver/receiver.py rtp --hook custom_lighting:process
# Included optional real-signal band-energy/heuristic onset demonstration:
python3 receiver/receiver.py rtp --hook features:process
```

`features.py` computes causal IIR bass/mid/treble energy proxies, RMS, peaks, transients and a simple energy-onset heuristic, emitting JSON lines up to20Hz. It is not a calibrated FFT/BPM detector. No native HyperHDR audio protocol compatibility is claimed; write the small adapter matching your engine's actual API. The basic receiver/streaming does not depend on these features.

## Latency measurement

RTP timestamp is a sample clock, not an absolute capture time. Even RTCP's read-completion wall-clock anchor cannot remove LG playback/DSP/ALSA buffering. To measure TV-to-receiver latency, use authorized impulse/tone content plus synchronized two-channel instrumentation (or an independently calibrated shared-clock acoustic/electrical test). Document capture-point vs TV-speaker acoustic delay separately. Do not infer latency from packet duration, ping RTT, or the configured jitter buffer alone.
