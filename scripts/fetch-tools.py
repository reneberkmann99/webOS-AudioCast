#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Download official, pinned build-only tools; verify bytes before extracting."""
import hashlib
import json
from pathlib import Path
import platform
import posixpath
import copy
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / '.tools'
LOCK = json.loads((ROOT / 'dependencies.lock.json').read_text())

def fetch(name, directory):
    item = LOCK[name]
    archive = TOOLS / item['url'].rsplit('/', 1)[1]
    TOOLS.mkdir(exist_ok=True)
    if not archive.exists():
        partial = archive.with_suffix(archive.suffix + '.partial')
        print('Download:', item['url'], flush=True)
        urllib.request.urlretrieve(item['url'], partial)
        partial.rename(archive)
    with archive.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    if digest != item['sha256']:
        raise SystemExit('SHA256 mismatch: ' + str(archive))
    target = TOOLS / directory
    if not target.exists():
        target.mkdir()
        with tarfile.open(archive) as tar:
            def safe_filter(member, destination):
                m = copy.copy(member)
                if m.issym() and m.linkname.startswith('/'):
                    # Two known official SDK links are absolute. Relocate inside extraction root;
                    # never create host filesystem links or loosen the standard safety filter.
                    old = '/__w/buildroot-nc4/buildroot-nc4/output/host/'
                    if m.linkname.startswith(old):
                        absolute_target = m.name.split('/')[0] + '/' + m.linkname[len(old):]
                    elif '/sysroot/' in m.name:
                        absolute_target = m.name.split('/sysroot/')[0] + '/sysroot' + m.linkname
                    else:
                        raise ValueError('Unsafe absolute SDK symlink: ' + m.name)
                    m.linkname = posixpath.relpath(absolute_target, posixpath.dirname(m.name))
                return tarfile.data_filter(m, destination)
            tar.extractall(target, filter=safe_filter)
    return target

if __name__ == '__main__':
    if platform.system() != 'Linux' or platform.machine() not in ('x86_64', 'amd64'):
        raise SystemExit('Pinned build host is Linux x86_64. Build via an x86_64 Linux VM/CI; no silent SDK substitution.')
    sdk = fetch('openlgtv/buildroot-nc4', 'sdk') / 'arm-webos-linux-gnueabi_sdk-buildroot'
    subprocess.run([str(sdk / 'relocate-sdk.sh')], check=True)
    ares = fetch('webosbrew/ares-cli-rs', 'ares')
    print('SDK:', sdk)
    print('ares-package:', next(ares.rglob('ares-package')))
