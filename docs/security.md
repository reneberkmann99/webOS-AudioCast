# Security and privacy model

AudioCast copies potentially private television audio onto a network. Only capture authorized content and comply with local law and household privacy expectations. The TV never records PCM to disk; the receiver records only an explicitly requested finite WAV. Debug reports contain system/model identifiers: review before sharing.

## Boundaries

- GUI is unprivileged; privileged startup uses Homebrew's existing root exec capability with a fixed installed path and enumerated action. No user text/destination/device becomes a shell command.
- Native daemon needs root only for firmware device permissions/persistent management. The network parser runs in that small process, so root-network exposure is a residual risk; do not internet-expose it. A future qualified drop-to-user/sandbox split is desirable but not implemented blindly across vendor device permissions/reopen behavior.
- Control7071 is hard-bound to IPv4loopback; random256-bitcontrol bearer required. Stream7070 defaults loopback; explicit LAN enable requires a different256-bitstream bearer. LAN stream listener has no control routes. No unauthenticated metadata/audio routes.
- Control bearer is returned only through local Homebrew bootstrap, retained in GUI memory, never URL/localStorage. The stream bearer is shown only by an explicit user action. Files0600/directory0700; OS `/dev/urandom`, not predictable RNG. Source release/IPK exclude credentials/config state.
- Local file-origin CORS supports the TV web app but does not authenticate by origin. External sites lack the bearer; no wildcard origin and no query credentials are accepted. Protect root/Homebrew access because Homebrew exec itself is a powerful existing trust boundary.
- Requests capped16KiB/8KiBbody and finite read deadline. Duplicate headers and ambiguous/chunked request framing rejected. At most8stream/pending clients with fixed per-client buffers. Allocation parameters/PCM format/device strings/ports are strictly validated; arbitrary ALSA plugin strings/default devices and shell metacharacters are rejected.

## Deliberate transport limits

Standard RTP/L16 is **not encrypted or authenticated**. The UI warns about this; no misleading bearer/RTP authentication claim. UDP destinations must resolve to private/link-local/loopback unicast. This reduces accidental internet broadcasting, not LAN eavesdropping/spoofing. IPv4subnet-directed broadcast cannot be proven for every custom mask from address syntax alone: configure a specific trusted receiver host, not a broadcast route. No multicast is offered.

HTTP7070 is cleartext unless protected by an external trusted VPN/network. Bearer/audio can be observed by an on-path host. The application does not open firewall ports, enable UPnP, install a reverse proxy/certificate, or publish an internet endpoint. A trusted isolated VLAN or VPN is recommended.

The reference receiver disables ambient HTTP proxies and redirects to avoid credential forwarding. Its local `--hook` imports trusted user-selected Python code, never network code. Optional speaker playback and WAV writing run on the receiver computer, not the TV.

## Residual/unqualified risks

- Experimental DILE capture temporarily changes **upload-only gain**, not speaker volume/routing,
  after separate explicit consent. It is restricted to the inspected library build and a 30-second
  manual grant; no DILE autostart, retry or ALSA fallback. A private gain-recovery record is committed
  before the setter. Normal cleanup verifies restoration after StopUpload; the supervisor attempts
  bounded owner-only recovery after an interrupted owner exits and quarantines on failure. SIGKILL,
  power loss or a wedged HAL can still require manual recovery/reboot; a timer is not a guarantee
  against an uninterruptible kernel hang. Active Bluetooth/ACR coexistence is not qualified.
- DILE library size/FNV fingerprints and a symbol-offset check are **compatibility guards**, not
  cryptographic code authenticity. Protect the existing root/HAL trust boundary. ABI fixture loading
  requires an explicit test-only CLI opt-in and is never packaged as a TV library.
- Rooted TV firmware/vendor ALSA driver behavior is not qualified. Opening even a capture-only device may have SoC-specific side effects; no routing invariance can be promised without testing.
- No DRM bypass, protected partition modification, firmware patching, proprietary library redistribution, microphone fallback, or hidden recording.
- Linux resource/socket timeouts cannot guarantee that a buggy kernel/vendor driver never blocks an ioctl. Supervisor offers bounded process shutdown; uninterruptible kernel waits require platform-specific investigation, not a firmware workaround here.
- DNS timeout behavior comes from the TV resolver. Use numeric receiver IP for unattended low-latency streaming. No claim that a successful DNS lookup/socket creation proves delivery.
- Automatic standby subscription, actual Homebrew process lifetime/cgroup behavior, full TV source compatibility and independent human security review remain pending.

## Rotate credentials / report issues

Stop and disable the service first. Back up configuration privately. With no AudioCast process running, remove only its protected credentials file and reopen the app to generate new tokens; update receiver token files. Never post the old/new tokens in bug reports. Keep `credentials.json`, recordings and raw private diagnostics out of Git and release archives.

For a report, share sanitized diagnostics, model/firmware/backend, exact reproduction, logs without tokens/content, and whether the issue occurs with authorized test audio. No hosted issue tracker is invented by this local prerelease; set the real repository/security contact before publication.
