#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Create reproducible source release, never caches/research/credentials/recordings."""
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import tarfile

ROOT=Path(__file__).resolve().parents[1]
INCLUDE=['app','daemon','service','receiver','scripts','tests','tools','docs','.github','third_party',
         'README.md','LICENSE','NOTICE.md','VERSION','dependencies.lock.json','package.json',
         'package-lock.json','.gitignore','.clang-format']

def main():
    version=(ROOT/'VERSION').read_text().strip();prefix='webos-audiocast-'+version
    files=[]
    for name in INCLUDE:
        item=ROOT/name
        if not item.exists():continue
        paths=item.rglob('*') if item.is_dir() else [item]
        for p in paths:
            if p.is_symlink():raise ValueError('source symlinks not allowed: '+str(p))
            if not p.is_file() or '__pycache__' in p.parts or p.suffix in ('.pyc','.wav','.pcm','.raw'):continue
            if p.name in ('credentials.json','.env'):raise ValueError('private file in source scope')
            files.append(p)
    epoch=int(os.environ.get('SOURCE_DATE_EPOCH','1792540800'))
    raw=io.BytesIO()
    with tarfile.open(fileobj=raw,mode='w',format=tarfile.USTAR_FORMAT) as tar:
        for p in sorted(files):
            info=tar.gettarinfo(str(p),prefix+'/'+str(p.relative_to(ROOT)))
            info.uid=info.gid=0;info.uname=info.gname='root';info.mtime=epoch
            info.mode=0o755 if os.access(p,os.X_OK) else 0o644
            with p.open('rb') as f:tar.addfile(info,f)
    out=ROOT/'dist'/(prefix+'-source.tar.gz');out.parent.mkdir(exist_ok=True)
    out.write_bytes(gzip.compress(raw.getvalue(),compresslevel=9,mtime=0))
    print(json.dumps({'source_archive':out.name,'files':len(files),'bytes':out.stat().st_size,
                      'sha256':hashlib.sha256(out.read_bytes()).hexdigest()},indent=2))

if __name__=='__main__':main()
