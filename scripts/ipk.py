#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Deterministically normalize an ares-generated webOS IPK and validate its payload."""
import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import tarfile

MAGIC = b'!<arch>\n'

def read_ar(data):
    if not data.startswith(MAGIC):
        raise ValueError('not an ar-format IPK')
    pos = 8
    entries = []
    while pos < len(data):
        header = data[pos:pos+60]
        if len(header) != 60 or header[58:] != b'`\n':
            raise ValueError('invalid ar member')
        name = header[:16].decode().strip().rstrip('/')
        size = int(header[48:58])
        pos += 60
        body = data[pos:pos+size]
        if len(body) != size:
            raise ValueError('truncated ar member')
        entries.append((name, body))
        pos += size + (size % 2)
    return entries

def write_ar(entries, epoch):
    out = bytearray(MAGIC)
    for name, body in entries:
        header = (f'{name+"/":<16}{epoch:<12}{0:<6}{0:<6}{"100644":<8}{len(body):<10}`\n').encode()
        if len(header) != 60:
            raise ValueError('ar member name too long')
        out.extend(header); out.extend(body)
        if len(body) % 2:
            out.extend(b'\n')
    return bytes(out)

def normalize_tar(data, epoch):
    source = tarfile.open(fileobj=io.BytesIO(data), mode='r:gz')
    out = io.BytesIO()
    with tarfile.open(fileobj=out, mode='w', format=tarfile.USTAR_FORMAT) as target:
        for member in sorted(source.getmembers(), key=lambda x: x.name):
            if Path(member.name).is_absolute() or '..' in Path(member.name).parts:
                raise ValueError('unsafe package path')
            member.uid = member.gid = 0
            member.uname = member.gname = 'root'
            member.mtime = epoch
            member.pax_headers = {}
            target.addfile(member, source.extractfile(member) if member.isfile() else None)
    return gzip.compress(out.getvalue(), compresslevel=9, mtime=0)

def normalize(path, epoch):
    members = read_ar(path.read_bytes())
    expected = {'debian-binary', 'control.tar.gz', 'data.tar.gz'}
    if {name for name, _ in members} != expected:
        raise ValueError('unexpected IPK members')
    canonical = []
    for name in ['debian-binary', 'control.tar.gz', 'data.tar.gz']:
        content = dict(members)[name]
        canonical.append((name, normalize_tar(content, epoch) if name.endswith('.tar.gz') else content))
    path.write_bytes(write_ar(canonical, epoch))
    validate(path)

def validate(path):
    members = dict(read_ar(path.read_bytes()))
    if members.get('debian-binary') != b'2.0\n':
        raise ValueError('incorrect package format')
    with tarfile.open(fileobj=io.BytesIO(members['data.tar.gz']), mode='r:gz') as data:
        paths = {m.name.lstrip('./'): m for m in data.getmembers()}
        prefix = 'usr/palm/applications/org.webosbrew.audiocast/'
        for name in ['appinfo.json','index.html','bin/audiocastd','service/startup/manage.sh',
                     'service/startup/boot-hook.sh','LICENSE']:
            if prefix+name not in paths:
                raise ValueError('missing package payload: ' + name)
        app = json.load(data.extractfile(paths[prefix+'appinfo.json']))
        if app['id'] != 'org.webosbrew.audiocast':
            raise ValueError('unexpected app ID')
        binary = data.extractfile(paths[prefix+'bin/audiocastd']).read()
        if binary[:6] != b'\x7fELF\x01\x01' or binary[18:20] != b'\x28\x00':
            raise ValueError('package must contain 32-bit little-endian ARM binary, never host x86')
        for name in ['bin/audiocastd','service/startup/manage.sh','service/startup/boot-hook.sh']:
            if not paths[prefix+name].mode & 0o111:
                raise ValueError('non-executable helper: ' + name)
        for name in paths:
            if any(x in name for x in ('credentials.json','.tools/', '.git/', '__pycache__')):
                raise ValueError('unexpected private/build file: '+name)
    print(json.dumps({'ipk':path.name,'bytes':path.stat().st_size,
                      'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
                      'app_id':app['id'],'version':app['version'],
                      'native_arch':'ARMv7 EABI soft-float, NEON','tv_install_tested':False}, indent=2))

if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('path', type=Path)
    parser.add_argument('--validate-only', action='store_true')
    args=parser.parse_args()
    if args.validate_only: validate(args.path)
    else: normalize(args.path, int(os.environ.get('SOURCE_DATE_EPOCH','1792540800')))
