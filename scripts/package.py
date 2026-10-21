#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Stage only reviewed application assets; use current Homebrew ares packager."""
import base64
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
DIST = ROOT / 'dist'
STAGE = ROOT / 'build/package/app'

if __name__ == '__main__':
    os.chdir(ROOT)
    binary = ROOT / 'build/webos/audiocastd'
    if not binary.is_file():
        raise SystemExit('Build the webOS binary first: scripts/build.sh webos')
    if STAGE.exists():
        shutil.rmtree(STAGE)  # Only this script-owned generated directory.
    shutil.copytree(ROOT / 'app', STAGE, ignore=shutil.ignore_patterns('bin', '*.map', '__pycache__'))
    shutil.copytree(ROOT / 'service', STAGE / 'service')
    (STAGE / 'bin').mkdir(exist_ok=True)
    shutil.copy2(binary, STAGE / 'bin/audiocastd')
    (STAGE / 'tools').mkdir(exist_ok=True)
    shutil.copy2(ROOT / 'scripts/diagnose.sh', STAGE / 'tools/diagnose.sh')
    for name in ['LICENSE', 'NOTICE.md']:
        shutil.copy2(ROOT / name, STAGE / name)
    (STAGE / 'licenses').mkdir(exist_ok=True)
    shutil.copy2(ROOT / 'third_party/nlohmann-LICENSE.MIT', STAGE / 'licenses/nlohmann-LICENSE.MIT')
    for path in STAGE.rglob('*'):
        if path.is_file():
            path.chmod(0o755 if path.suffix == '.sh' or path.name == 'audiocastd' else 0o644)
            os.utime(path, (int(os.environ.get('SOURCE_DATE_EPOCH', '1792540800')),)*2)
    tools = list((ROOT / '.tools/ares').rglob('ares-package'))
    if not tools:
        subprocess.run([sys.executable, 'scripts/fetch-tools.py'], check=True)
        tools = list((ROOT / '.tools/ares').rglob('ares-package'))
    DIST.mkdir(exist_ok=True)
    subprocess.run([str(tools[0]), str(STAGE), '--force-arch', 'arm', '--outdir', str(DIST)], check=True)
    app = json.loads((STAGE / 'appinfo.json').read_text())
    ipk = DIST / (app['id'] + '_' + app['version'] + '_arm.ipk')
    subprocess.run([sys.executable, 'scripts/ipk.py', str(ipk)], check=True)
    icon = STAGE / app['icon']
    manifest = {k: app[k] for k in ['id', 'version', 'type', 'title']}
    manifest.update({
        'appDescription': 'Hardware-unqualified engineering prerelease. Root required. ALSA playback capture must be validated per TV.',
        'iconUri': 'data:image/png;base64,' + base64.b64encode(icon.read_bytes()).decode(),
        'rootRequired': True, 'ipkUrl': ipk.name,
        'ipkHash': {'sha256': hashlib.sha256(ipk.read_bytes()).hexdigest()},
        'ipkSize': ipk.stat().st_size,
        'installedSize': sum(p.stat().st_size for p in STAGE.rglob('*') if p.is_file())})
    (DIST / (app['id'] + '.manifest.json')).write_text(json.dumps(manifest, indent=2)+'\n')
    (DIST / 'SHA256SUMS').write_text(manifest['ipkHash']['sha256'] + '  ' + ipk.name + '\n')
    print('Packaged:', ipk)
    print('TV installation/capture NOT verified; see docs/validation.md')
