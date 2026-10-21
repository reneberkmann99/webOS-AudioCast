# Installation, updates, recovery and removal

**0.1.0 is a hardware-unqualified engineering prerelease, not a production-qualified TV release.** The IPK has been cross-built and structurally validated, but not installed on a physical LG TV. You need an already-rooted compatible TV and a functioning Homebrew Channel root service. This project does not root TVs, alter firmware, or install boot exploits.

## 1. Collect inventory before opening capture hardware

Copy the included diagnostic script to a TV you are authorized to administer:

```sh
scp scripts/diagnose.sh root@TV_IP:/tmp/audiocast-diagnose.sh
ssh root@TV_IP 'sh /tmp/audiocast-diagnose.sh' > tv-inventory.txt
```

This inventories ALSA/devices/mixers read-only, versions, utilities/libraries, root service and best-effort output mode. Potentially blocking commands are timed out or skipped if a timeout utility is absent. No credentials or audio are saved. Review identifiers before sharing the report. No capture device is opened by default.

After inspecting the report, optionally query ONE explicit candidate's capabilities:

```sh
# Once the app binary is installed; replace CARD,DEVICE, do not assume 0,13.
ssh root@TV_IP 'sh /media/developer/apps/usr/palm/applications/org.webosbrew.audiocast/tools/diagnose.sh --device hw:CARD,DEVICE'
```

The native probe opens capture only to query independent constraints; it does not apply capture parameters or read PCM. Opening a vendor device can still have firmware-specific effects, so opt in deliberately. No arbitrary capture/microphone scanning occurs.

## 2. Install the ARM package

Build host is Linux x86_64, with Python>=3.12, C++ compiler, Node>=20 for development tests. Pinned official tools download automatically during cross-build:

```sh
./scripts/build.sh package
# Creates dist/org.webosbrew.audiocast_0.1.0_arm.ipk and a Homebrew manifest.
```

Alternatively use the provided prebuilt IPK. Verify its SHA256 against `dist/SHA256SUMS`. Configure a root-capable TV device using webOS Dev Manager / the normal webOS SDK device setup. Then:

```sh
./scripts/deploy.sh YOUR_CONFIGURED_DEVICE dist/org.webosbrew.audiocast_0.1.0_arm.ipk
# Open AudioCast from the TV launcher. Installation does not auto-launch/stream.
```

A manual root SSH installation using the documented Homebrew install API is also possible:

```sh
scp dist/org.webosbrew.audiocast_0.1.0_arm.ipk root@TV_IP:/tmp/
ssh -t root@TV_IP "luna-send-pub -i -f luna://com.webos.appInstallService/dev/install '{\"id\":\"org.webosbrew.audiocast\",\"ipkUrl\":\"/tmp/org.webosbrew.audiocast_0.1.0_arm.ipk\",\"subscribe\":true}'"
```

Observe the final successful installation event, not merely an initial subscription acknowledgement. End the interactive subscription after completion. These are on-device instructions, not a claim that this install was tested. This build contains ARMv7 NEON EABI/soft-float code; a 64-bit-only TV without a tested32-bit compatibility runtime is not supported by this binary. We do not silently install a bridge, firmware library, or system modification.

## 3. Validate playback before enabling unattended streaming

1. Open AudioCast. Its fixed Homebrew bootstrap starts an **idle detached daemon**, generates root-only local tokens, and connects the UI to the authenticated loopback API. If it fails, check root status in Homebrew Channel, architecture/library errors, and logs; Developer Mode alone is insufficient.
2. Open Configuration. Select an inventoried candidate, or enter an explicit `hw:CARD,DEVICE` using the remote's on-screen editor. Device names are hints, not proof. Never select a microphone.
3. Choose S16_LE,48kHz,stereo unless the native capabilities require another supported combination. Stop before changes. Apply with playback confirmation **off** initially.
4. Start known, authorized TV playback with distinct sound/silent sections. Press **Test audio capture ·5sec**. This reads real PCM for meters only; no network audio or TV recording. Confirm level changes correlate with the known playback. An open device or nonzero noise alone is insufficient.
5. Run the receiver on your computer (next section). Return to Configuration, explicitly confirm the known playback source, set receiver's private LAN IP and UDP port, apply, and press Start. This explicit streaming step allows audible/sample-level verification; if the samples are not the authorized TV content, stop and revoke confirmation. Start acceptance itself is not proof of success.
6. Verify TV speakers continue normally, GUI closure leaves the stream running, source/output switches behave as expected, and receiver PCM actually matches the TV. Record each source/output combination in `docs/compatibility.md` before relying on it.
7. Only after qualification, enable streaming at boot. Apply creates a **symlink** `/var/lib/webosbrew/init.d/audiocast` targeting the installed app's boot hook. The native config also requires `autostart:true` and `playback_confirmed:true`. Test reboot/standby yourself; automatic power-event subscription is not yet implemented.

Arrows navigate; OK activates buttons/opens the text editor; left/right changes a focused selector. Back closes the editor or returns to Dashboard, then exits the app. Closing the GUI never sends Stop.

## 4. Reference receiver

RTP is the default low-latency mode. Open inbound UDP5004 on your **computer's** firewall if needed; AudioCast does not alter firewalls.

```sh
python3 receiver/receiver.py rtp --bind 0.0.0.0 --port 5004 --rate 48000 --channels 2
# Optional playback; requires PortAudio/sounddevice on your computer:
python3 -m pip install -r receiver/requirements.txt
python3 receiver/receiver.py rtp --port 5004 --rate 48000 --channels 2 --play
```

For HTTP, select HTTP/Both and explicitly enable LAN access. Show the private stream credential in Configuration and store **only the token** in a protected receiver file, never in a URL, screenshot or public log:

```sh
chmod 600 stream-token.txt  # POSIX; use equivalent file ACLs on Windows.
python3 receiver/receiver.py http --url http://TV_IP:7070/audio --token-file stream-token.txt
```

See `docs/receiver.md` for recording (explicit opt-in only), lighting hooks, framing, limitations and platform setup.

## 5. Update safely

Settings/tokens live outside the app in `/var/lib/webos-audiocast` and are preserved. **Before upgrading**, stop the service via root SSH:

```sh
ssh root@TV_IP 'sh /media/developer/apps/usr/palm/applications/org.webosbrew.audiocast/service/startup/manage.sh stop'
# Wait for process shutdown, then install the new IPK using Dev Manager/ares-install.
```

Keep the old IPK for rollback. Reopen the app after installation; review changed capture semantics/formats before re-confirming. Do not assume an automatic installer replacement restarts an already-running old daemon. The documented stop/install/reopen path is the supported upgrade procedure; real TV upgrade testing is pending.

## 6. Disable boot, stop and uninstall

Uncheck autostart and Apply in the GUI, or use fixed helpers before removal:

```sh
ssh root@TV_IP 'sh /media/developer/apps/usr/palm/applications/org.webosbrew.audiocast/service/startup/manage.sh disable'
ssh root@TV_IP 'sh /media/developer/apps/usr/palm/applications/org.webosbrew.audiocast/service/startup/manage.sh stop'
# Remove using Homebrew Channel/Dev Manager or configured ares-install:
ares-install -d YOUR_CONFIGURED_DEVICE --remove org.webosbrew.audiocast
```

Direct app deletion leaves at most a **dangling symlink**, not an executable startup script; the running supervisor checks app-binary existence every~2sec and terminates the child when removed. Host-tested; actual Homebrew lifecycle/power behavior is unverified. Configuration/private tokens remain intentionally for rollback and must be deleted deliberately if purging. Do not publish or copy `credentials.json` into a source tree.

## 7. Recovery without firmware changes

- Stop/disable through the fixed helper, reinstall the prior IPK, reopen.
- Repeated short crashes create `quarantined`; Reconnect explicitly clears this and retries.
- If an interrupted initial supervisor launch leaves `supervisor.lock` with no owner, first verify no AudioCast supervisor/daemon is running; only then remove that stale lock directory. Do not blindly delete active locks or kill PIDs without checking `/proc/PID/cmdline`.
- Boot recovery: remove **only AudioCast's symlink** from Homebrew init.d. Never modify LG system partitions, boot components, audio daemon binaries, or global Homebrew startup files.
