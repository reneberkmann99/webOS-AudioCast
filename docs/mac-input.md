# TV audio as a macOS input device (routable to AirPods)

Expose the TV's own playback audio as an **input device** on a Mac, then route it
anywhere (AirPods, a recorder, an app, etc.). This is the listening/companion half of
the AudioCast project (the other half drives the HyperHDR backlight).

## How it works

```
webOS TV (AudioCast)                     MacBook
┌───────────────────────┐               ┌──────────────────────────────────────┐
│ DILE capture (48k     │               │  scripts/tv-audio-mac.sh             │
│ stereo S16)           │──HTTP chunked─▶│  └─ receiver.py http --play         │
│ daemon HTTP listener  │   raw PCM     │       --device "<output>"            │
│ 10.10.40.104:7070     │  (Bearer auth)│         │                            │
└───────────────────────┘               │         ▼                            │
                                        │  BlackHole (loopback)  → "input device"│
                                        │  or AirPods            → hear TV      │
                                        │  or Multi-Output [BlackHole+AirPods]  │
                                        └──────────────────────────────────────┘
```

The AudioCast daemon already serves a raw-PCM HTTP stream (`GET /audio`) with a bearer
token. The Mac pulls it and plays it into a CoreAudio **output** device of your choice.
Playing into a **BlackHole** loopback device is what makes the TV audio appear as an
**input** device (BlackHole mirrors its output side onto its input side).

## Prerequisites (one-time, on the TV)

1. TV must be serving HTTP audio, not just RTP. In the AudioCast app set the transport
   to include HTTP and enable **LAN access**, or set it via the daemon control API:
   ```bash
   # on the TV (or from the gateway via ssh root@10.10.40.104)
   curl -s -X POST -H "Authorization: Bearer $CONTROL_TOKEN" \
     -H 'Content-Type: application/json' --data '{"transport":"both","http_lan":true}' \
     http://127.0.0.1:7071/v1/config
   ```
2. Start the capture as usual (open AudioCast → `mstar-upload:0` → both consent boxes →
   **Start**). The HTTP stream is only active while capture is running.

## Mac side

### Install the one dependency

```bash
brew install portaudio
pip3 install sounddevice
```

### List available output devices

```bash
scripts/tv-audio-mac.sh list-devices
# 0: BlackHole 2ch (2 ch)
# 1: MacBook Air Speakers (2 ch)
# 3: Rene's AirPods Pro (2 ch)
```

### Run it

```bash
# TV as an INPUT device (for any app / routing)
scripts/tv-audio-mac.sh "BlackHole 2ch"

# Just listen to the TV on AirPods
scripts/tv-audio-mac.sh "Rene's AirPods Pro"

# Both: create a Multi-Output Device [BlackHole + AirPods] in Audio MIDI Setup,
# then target its name
scripts/tv-audio-mac.sh "Multi-Output Device"
```

First run fetches the stream token from the TV automatically (`ssh root@10.10.40.104`),
so the Mac needs SSH access to the TV once (or set `TOKEN_FILE` manually).

### Auto-start at login (input-device mode)

```bash
mkdir -p ~/Library/LaunchAgents
# edit the two paths inside the plist to point at this repo
cp deploy/com.berkmann.tvaudio.plist ~/Library/LaunchAgents/
launchctl load ~/Library/LaunchAgents/com.berkmann.tvaudio.plist
```

## Routing to AirPods (the "send it where I want" part)

- **Direct:** run the launcher with the AirPods device name (see above).
- **Input + listen at once:** Audio MIDI Setup → `+` → **Create Multi-Output Device** →
  tick BlackHole 2ch and your AirPods → target that device in the launcher. TV audio then
  reaches both BlackHole (input, for other apps) and AirPods (your ears).
- **Input only, then manually route:** keep it on `BlackHole 2ch` and pick BlackHole as the
  input in whichever app you want (QuickTime, Discord, Logic, OBS, …).

## Notes / limits

- The stream is **48 kHz stereo S16 LE** raw PCM. Latency is one jitter-buffer (30 ms)
  plus the macOS device buffer — fine for TV listening.
- The HTTP stream uses a **bearer token** and is **cleartext** (no TLS); keep it on the
  trusted home LAN.
- The Mac must reach the TV (`10.10.40.104:7070`) — same LAN, or via your WireGuard/VPN.
- TV capture is DILE-based and must be started explicitly (no auto-start); see the main
  README / `docs/dile-pcm-api.md`.
