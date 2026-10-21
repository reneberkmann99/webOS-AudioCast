# Installation, updates, recovery and removal

**0.1.1 is an engineering prerelease, not a playback-qualified TV release.** Native execution, app installation and boot-service startup have been tested on one MStar TV. Media playback capture on that TV is **not working**; unsafe MAD capture devices are blocked. Other models remain unqualified. You need an already-rooted compatible TV and a functioning Homebrew Channel root service. This project does not root TVs, alter firmware, or install boot exploits.

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
# Creates dist/org.webosbrew.audiocast_0.1.1_arm.ipk and a Homebrew manifest.
```

Alternatively use the provided prebuilt IPK. Verify its SHA256 against `dist/SHA256SUMS`. Configure a root-capable TV device using webOS Dev Manager / the normal webOS SDK device setup. Then:

```sh
./scripts/deploy.sh YOUR_CONFIGURED_DEVICE dist/org.webosbrew.audiocast_0.1.1_arm.ipk
# Open AudioCast from the TV launcher. Installation does not auto-launch/stream.
```

A manual root SSH installation using the documented Homebrew install API is also possible:

```sh
scp dist/org.webosbrew.audiocast_0.1.1_arm.ipk root@TV_IP:/tmp/
ssh -t root@TV_IP "luna-send-pub -i -f luna://com.webos.appInstallService/dev/install '{\"id\":\"org.webosbrew.audiocast\",\"ipkUrl\":\"/tmp/org.webosbrew.audiocast_0.1.1_arm.ipk\",\"subscribe\":true}'"
```

Observe the final successful installation event, not merely an initial subscription acknowledgement. End the interactive subscription after completion. The ares installation path has been tested on the MStar TV; this alternative CLI recipe is not independently qualified. This build contains ARMv7 NEON EABI/soft-float code; a 64-bit-only TV without a tested32-bit compatibility runtime is not supported by this binary. We do not silently install a bridge, firmware library, or system modification.

## 3. Validate playback before enabling unattended streaming

1. Open AudioCast. Its fixed Homebrew bootstrap starts an **idle detached daemon**, generates root-only local tokens, and connects the UI to the authenticated loopback API. If it fails, check root status in Homebrew Channel, architecture/library errors, and logs; Developer Mode alone is insufficient.
2. In **Receiver destination**, enter the receiver IP/hostname and UDP port, then save. OK opens a D-pad-navigable editor. In **Playback capture source**, select a safe inventoried device. Unsafe devices and microphones are disabled. No device is automatically selected or confirmed. On the tested MStar TV no supported source exists, so streaming is visibly blocked.
3. Advanced settings contains the sample format/rate/channels and HTTP LAN-access checkbox. Default is 16-bit PCM, 48 kHz stereo. Stop capture before changing settings. Save with playback confirmation **off** initially.
4. Start known, authorized TV playback with distinct sound/silent sections. Press **Test audio capture ·5sec**. This reads real PCM for meters only; no network audio or TV recording. Confirm level changes correlate with the known playback. An open device or nonzero noise alone is insufficient.
5. Run the receiver on your computer (next section). Explicitly confirm the known playback source, save the receiver's private LAN IP and UDP port, and press Start. This explicit streaming step allows audible/sample-level verification; if the samples are not the authorized TV content, stop and revoke confirmation. Start acceptance itself is not proof of success.
6. Verify TV speakers continue normally, GUI closure leaves the stream running, source/output switches behave as expected, and receiver PCM actually matches the TV. Record each source/output combination in `docs/compatibility.md` before relying on it.
7. Opening the app enables the **background service** boot symlink `/var/lib/webosbrew/init.d/audiocast`, independently of automatic streaming. The service starts idle when stream autostart is off. Only after playback qualification, check **Start streaming automatically when the TV boots**, then save. Automatic streaming also requires `autostart:true` and `playback_confirmed:true`. Full-reboot service startup has been observed on the tested TV; standby/wake and automatic power-event recovery are not qualified.

Arrows navigate; OK activates buttons/opens the text editor; left/right changes a focused selector. Back cancels the editor, closes advanced settings/diagnostics, or exits the app. Closing the GUI never sends Stop.

## 4. Reference receiver

RTP is the default low-latency mode. Open inbound UDP5004 on your **computer's** firewall if needed; AudioCast does not alter firewalls.

```sh
python3 receiver/receiver.py rtp --bind 0.0.0.0 --port 5004 --rate 48000 --channels 2
# Optional playback; requires PortAudio/sounddevice on your computer:
python3 -m pip install -r receiver/requirements.txt
python3 receiver/receiver.py rtp --port 5004 --rate 48000 --channels 2 --play
```

For HTTP, select HTTP/Both and explicitly enable LAN access in Advanced settings. Retrieve **only the stream token** using root SSH into a protected file on your computer, never a URL, screenshot or public log:

```sh
umask 077
ssh root@TV_IP 'cat /var/lib/webos-audiocast/credentials.json' | python3 -c 'import sys,json; print(json.load(sys.stdin)["stream_token"])' > stream-token.txt
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

Keep the old IPK for rollback. Reopen the app after installation; review changed capture semantics/formats before re-confirming. Do not assume an automatic installer replacement restarts an already-running old daemon. The stop/install/reopen path was used on the real TV for this GUI upgrade; it does not qualify audio capture or future firmware compatibility.

## 6. Disable boot, stop and uninstall

Uncheck automatic streaming and save to stop streams auto-starting; this intentionally leaves the background service boot-enabled. To disable the service itself, use the fixed helper before removal:

```sh
ssh root@TV_IP 'sh /media/developer/apps/usr/palm/applications/org.webosbrew.audiocast/service/startup/manage.sh disable'
ssh root@TV_IP 'sh /media/developer/apps/usr/palm/applications/org.webosbrew.audiocast/service/startup/manage.sh stop'
# Remove using Homebrew Channel/Dev Manager or configured ares-install:
ares-install -d YOUR_CONFIGURED_DEVICE --remove org.webosbrew.audiocast
```

Direct app deletion leaves at most a **dangling symlink**, not an executable startup script; the running supervisor checks app-binary existence every~2sec and terminates the child when removed. Uninstall detection is host-tested; real TV uninstall/standby behavior is not qualified. Configuration/private tokens remain intentionally for rollback and must be deleted deliberately if purging. Do not publish or copy `credentials.json` into a source tree.

## 7. Recovery without firmware changes

- Stop/disable through the fixed helper, reinstall the prior IPK, reopen.
- Repeated short crashes create `quarantined`; Reconnect explicitly clears this and retries.
- If an interrupted initial supervisor launch leaves `supervisor.lock` with no owner, first verify no AudioCast supervisor/daemon is running; only then remove that stale lock directory. Do not blindly delete active locks or kill PIDs without checking `/proc/PID/cmdline`.
- Boot recovery: remove **only AudioCast's symlink** from Homebrew init.d. Never modify LG system partitions, boot components, audio daemon binaries, or global Homebrew startup files.
