# TV → HyperHDR reactive-backlight deployment

Deploys the AudioCast TV audio stream into the existing HyperHDR techno engine on
`vm-docker` (`10.10.100.101`), so the TV's own playback audio drives the LED backlight.

## Pipeline

```
webOS TV                          vm-docker (10.10.100.101)
AudioCast daemon (DILE backend)   ──RTP/L16 48kHz stereo S16──▶  tv-audio-adapter.py (UDP 5004)
  mstar-upload:0, gain 127            (downmix mono, resample 48k→22050, forward float32 LE)
                                          │
                                          ▼ 127.0.0.1:17000
                                     techno engine (FFT) ──raw RGB UDP──▶ HyperHDR rawUdpServer :5568
                                          priority 100 (rawUdpServer minimum) → LED device
```

## Files

- `receiver/tv_audio_adapter.py` — the RTP→engine adapter (runs on vm-docker as a service).
- `hyperhdr-tv-audio-firewall.sh` — iptables: TV-only on UDP 5004, loopback+legacy-Mac on 17000.
- `hyperhdr-tv-audio.service` — systemd unit for the adapter (User=rene, Restart=always).

## Deploy (on vm-docker, as root)

```sh
install -m 755 hyperhdr-tv-audio-firewall.sh /usr/local/sbin/hyperhdr-audio-firewall.sh
install -m 644 hyperhdr-tv-audio.service /etc/systemd/system/hyperhdr-tv-audio.service
/usr/local/sbin/hyperhdr-audio-firewall.sh
systemctl daemon-reload
systemctl enable --now hyperhdr-tv-audio.service
```

## HyperHDR priority

The rawUdpServer source has a **minimum priority of 100** (values below auto-correct to
the default 109). Set it to 100 in the HyperHDR DB (`settings.type='rawUdpServer'`) and keep
the techno engine's `raw_udp_priority` and `hyperhdr-music.sh` `engine_priority` at 100 too.

## TV side

AudioCast config: backend `dile`, device `mstar-upload:0`, `dile_gain_confirmed=true`,
`playback_confirmed=true`, `dile_continuous=true`, destination `10.10.100.101:5004`,
transport `rtp`, `autostart=false` (DILE forbids auto-start). Start capture explicitly
(open AudioCast → mstar-upload:0 → both consent boxes → Start).
