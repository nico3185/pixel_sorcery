#!/usr/bin/env python3
"""export_build.py SRC DEST: put a finished build where a person finds it (make's EXPORT_DIR, by default
Documents/PixelSorcery), and print "OUTPUT: <path>" for the build screen's "Show" button.

SRC a .cue: the disc - the .cue and every file it names - into the folder DEST (emulators open the .cue);
SRC any other file (a ROM made from your dumps): copied to DEST, a folder or a file name.
On macOS the copy is an APFS clone (no extra space until one side changes). Nothing leaves this machine:
the outputs hold data from your own dumps and are for you only."""
import os, re, shutil, subprocess, sys


def copy(src, dst):
    if os.path.exists(dst): os.remove(dst)
    if sys.platform == 'darwin' and subprocess.run(['cp', '-c', src, dst], capture_output=True).returncode == 0:
        return
    shutil.copy2(src, dst)


def main():
    if len(sys.argv) != 3: sys.exit(__doc__)
    folder = sys.argv[2].endswith(('/', os.sep))          # DEST/ : a folder, the file keeps its name
    src, dst = os.path.abspath(sys.argv[1]), os.path.abspath(os.path.expanduser(sys.argv[2]))
    if not os.path.isfile(src): sys.exit(f'export_build.py: no {src}')
    if src.lower().endswith('.cue'):
        os.makedirs(dst, exist_ok=True)
        here = os.path.dirname(src)
        names = re.findall(r'^\s*FILE\s+"([^"]+)"', open(src, errors='replace').read(), re.M)
        for n in names:
            if not os.path.isfile(os.path.join(here, n)): sys.exit(f'export_build.py: {src} names {n}, not there')
        for n in names: copy(os.path.join(here, n), os.path.join(dst, n))
        out = os.path.join(dst, os.path.basename(src)); copy(src, out)
    else:
        out = os.path.join(dst, os.path.basename(src)) if folder or os.path.isdir(dst) else dst
        os.makedirs(os.path.dirname(out), exist_ok=True); copy(src, out)
    print(f'OUTPUT: {out}')


if __name__ == '__main__':
    main()
