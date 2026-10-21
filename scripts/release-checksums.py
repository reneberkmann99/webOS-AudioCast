#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Emit a complete release checksum manifest for all dist artifacts."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DIST = ROOT / 'dist'

def main():
    artifacts = sorted(p for p in DIST.iterdir() if p.is_file() and p.name != 'SHA256SUMS')
    if not artifacts:
        raise SystemExit('No dist artifacts; run scripts/build.sh package and scripts/source-release.py first')
    lines = []
    manifest = []
    for path in artifacts:
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        lines.append(f'{digest}  {path.name}')
        manifest.append({'file': path.name, 'sha256': digest, 'bytes': path.stat().st_size})
    (DIST / 'SHA256SUMS').write_text('\n'.join(lines) + '\n')
    (DIST / 'checksums.json').write_text(json.dumps({'artifacts': manifest}, indent=2) + '\n')
    print(json.dumps(manifest, indent=2))

if __name__ == '__main__':
    main()
