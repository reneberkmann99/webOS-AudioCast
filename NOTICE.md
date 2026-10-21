# Provenance and third-party notices

- AudioCast application/daemon/receiver/assets are new MIT-licensed code, developed with substantial AI assistance. They have not undergone meaningful independent human TV testing or release review yet. Do not represent this prerelease as ready for the central Homebrew repository.
- `third_party/json.hpp`: nlohmann/json **v3.11.3**, copyright Niels Lohmann, MIT. Full license: `third_party/nlohmann-LICENSE.MIT` (also bundled in the IPK). Pinned SHA-256 in `dependencies.lock.json`.
- Native ALSA access dynamically uses a TV-provided `libasound.so.2`. ALSA is generally LGPL-2.1-or-later; no ALSA library is redistributed in this IPK. ABI declarations and calls are new implementation code. No LG firmware/vendor library is redistributed.
- Native webOS builds statically link GCC libstdc++/libgcc under GPLv3 with the GCC Runtime Library Exception 3.1, permitting distribution of independently licensed programs. Buildroot SDK and ares-cli-rs are build tools only, downloaded separately and never included in the source release/IPK.
- Python receiver needs only the Python standard library for receive, levels, and WAV output. Optional sounddevice (MIT) is installed separately for playback and uses PortAudio; it is not bundled. Playwright (Apache-2.0) is a pinned development-only dependency, not an app runtime dependency.
- Architectural references: Homebrew Channel (MIT), PicCap (MIT), hyperion-webos (MIT), apps-repo documentation (Apache-2.0). No source files from these applications are embedded in AudioCast. API names, conventions, and independently written integration scripts follow their publicly documented interfaces.
- **nickvsnetworking/webos-audiostream-remote**: no license was declared in the repository snapshot inspected on 2026-10-21. Its code was **not reused**. The MARS CAPTURE_TEST command/report is used only as attributed feasibility evidence, never as universal compatibility proof.

Research source revisions and limitations are in `docs/research.md`. Raw downloaded research/source files and build tool archives are not release deliverables.
