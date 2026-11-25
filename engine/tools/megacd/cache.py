#!/usr/bin/env python3
"""cache.py: a content-addressed cache for the compilation disc build (evidence captures, zone images).

  cache.py key [-f FILE]... [-s STRING]...   print the key: SHA-256 of every FILE's bytes (a directory: its files,
                                              sorted by path) and every STRING, in order
  cache.py get KEY DEST...                    a hit copies the stored entries back (each DEST: a file or directory,
                                              as stored by put, by its name); exit 0 - a miss exits 1
  cache.py put KEY SRC...                     stores each SRC (file or directory) under KEY

The cache lives in $PS_BUILD_CACHE (default build/cache beside the checkout). NOCACHE=1: every get misses and put
stores nothing - a clean build, for releases. A key must name everything that shapes the output: the ROM, the
recording or movie, the tool binaries and scripts, and the options (environment) of the step; the scripts pass
them. Entries are written to a temporary directory and renamed, so an interrupted build leaves no half entry."""
import hashlib, os, shutil, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CACHE = Path(os.environ.get('PS_BUILD_CACHE', ROOT / 'build' / 'cache'))


def _hash_path(h, p):
    p = Path(p)
    if p.is_dir():
        for f in sorted(x for x in p.rglob('*') if x.is_file()):
            h.update(str(f.relative_to(p)).encode() + b'\0')
            h.update(f.read_bytes())
    elif p.exists():
        h.update(p.read_bytes())
    else:
        h.update(b'<missing>' + str(p).encode())


def key(args):
    h = hashlib.sha256()
    i = 0
    while i < len(args):
        a, v = args[i], args[i + 1]
        h.update(a.encode() + b'\0')
        if a == '-f': _hash_path(h, v)
        else: h.update(v.encode())
        h.update(b'\0'); i += 2
    return h.hexdigest()


def get(k, dests):
    if os.environ.get('NOCACHE'): return 1
    e = CACHE / k[:2] / k
    if not e.is_dir(): return 1
    for d in dests:
        src = e / Path(d).name
        if not src.exists(): return 1
    for d in dests:
        src, d = e / Path(d).name, Path(d)
        if d.is_dir(): shutil.rmtree(d)
        elif d.exists(): d.unlink()
        d.parent.mkdir(parents=True, exist_ok=True)
        (shutil.copytree if src.is_dir() else shutil.copy2)(src, d)
    return 0


def put(k, srcs):
    if os.environ.get('NOCACHE'): return 0
    e = CACHE / k[:2] / k
    if e.is_dir(): return 0
    e.parent.mkdir(parents=True, exist_ok=True)
    t = Path(tempfile.mkdtemp(dir=e.parent))
    for s in srcs:
        s = Path(s)
        if s.is_dir(): shutil.copytree(s, t / s.name)
        elif s.exists(): shutil.copy2(s, t / s.name)
    try: t.rename(e)
    except OSError: shutil.rmtree(t, ignore_errors=True)      # another build stored it first
    return 0


def main():
    cmd, a = sys.argv[1], sys.argv[2:]
    if cmd == 'key': print(key(a)); return 0
    if cmd == 'get': return get(a[0], a[1:])
    if cmd == 'put': return put(a[0], a[1:])
    sys.exit(__doc__)


if __name__ == '__main__':
    sys.exit(main())
