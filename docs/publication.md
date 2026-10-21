# Central Homebrew repository publication (NOT YET SUBMITTED)

## Current publication status

AudioCast is a local engineering prerelease. It is **not** listed in `webosbrew/apps-repo`, and no pull request has been opened. The central repository's own README (inspected 2026-10-21) explicitly rejects "applications made primarily by AI and submitted without meaningful human development, testing, and review," and requires AI-assistance disclosure. Those conditions are not met by an unreviewed, unqualified TV capture implementation.

## Preconditions before any submission

1. **Human TV capture validation.** Complete and evidence the worksheet in `docs/compatibility.md` on at least one real, compatible, authorized rooted TV. A reproduced capture path is the entire point of the app.
2. **Meaningful human development/review.** Review the native capture/network code and privileged lifecycle scripts for correctness and safety, not merely acceptance of generated output.
3. **Lifecycle/quality.** Reboot persistence, standby behavior, install/update/uninstall/rollback on real Homebrew, remote usability, and a multi-hour session.
4. **License compliance.** Retain `NOTICE.md`, bundled nlohmann license, and the dynamic-ALSA runtime note. Confirm no vendor/proprietary artifacts are in the IPK.
5. **Security/privacy review.** Confirm the loopback control + bearer model, cleartext RTP/HTTP warnings, and no recording default satisfy a reviewer on the target trust model.
6. **Real maintainers/contact.** The source `sourceUrl`, release manifest URLs, security/issue contact and sponsor metadata must reference real, reachable infrastructure; this repository currently has none configured.

## Required metadata (template)

When real hosting exists, `apps-repo/packages/<id>.yml` follows the documented schema:

```yaml
title: webOS AudioCast
iconUri: https://example.invalid/icon160.png          # replace with real, reachable HTTPS
manifestUrl: https://example.invalid/releases/latest/download/org.webosbrew.audiocast.manifest.json
category: system
pool: main
description: |
  Rooted LG webOS system-audio capture and LAN streaming.
  Requires explicit on-TV playback capture validation; never records; never modifies routing.
screenshots:
  - url: https://example.invalid/main.png
    caption: Dashboard
  - url: https://example.invalid/settings.png
    caption: Configuration
funding:
  github: [ audio-cast-maintainers ]                 # real handle(s)
```

The release manifest fields (id, version, type, title, appDescription, iconUri, rootRequired, ipkUrl, ipkHash{sha256}, ipkSize, installedSize) are already generated relative to the release by `scripts/package.py` and `scripts/release-checksums.py`. `rootRequired` must remain `true`.

## Review notes for the eventual PR

- Disclose that substantial portions were AI-assisted (see `NOTICE.md`).
- Do not submit a build whose `tv_install_tested` is still false; reviewers will test on hardware.
- Do not claim DRM, protected-content, HDMI/ARC, Bluetooth, or optical support that the worksheet did not establish.
- Provide exact reproduction of the validated capture device/format per model, so reviewers can independently confirm.
- Keep the startup hook a symlink and the uninstall path clean; reviewers check for orphaned boot scripts.
- Provide the sanitized diagnostic path and expected reviewer runbook.

## Release engineering (used by CI, not submission)

`scripts/build.sh package` → `scripts/source-release.py` → `scripts/release-checksums.py`; the workflow publishes a **prerelease only**. A `vX.Y.Z` tag is not a statement of TV qualification. Remove the "engineering prerelease" framing only after the preconditions above are satisfied and independently reviewed.
