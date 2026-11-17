#!/usr/bin/env python3
"""find_disc.py DIR [REGION] [--list]: print the first .cue under DIR (any depth) holding a SHCD disc of
REGION - US (the default), EU or JP, also U/E/J - recognised as SHCD by roms.json and placed in its region by
its boot sector's security code (as the Mega CD BIOS checks it). The compilation disc takes that disc's region:
US and JP run at 60 Hz, EU at 50 Hz; its SHCD card plays that disc's music. Prints nothing when there is none.
--list: every SHCD found, one "REGION path" a line. Used by the Makefile so a ROM folder is all `make` needs."""
import os, sys
args = [a for a in sys.argv[1:] if a != '--list']; listing = '--list' in sys.argv
sys.argv = sys.argv[:1]
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..')); sys.path.insert(0, HERE)
import build_ui, build_disc

CODE = {0xA1: 'JP', 0x64: 'EU'}                 # anything else: USA


def region(cue):
    try: return CODE.get(build_disc.read_data(cue, 0x800)[0x20B], 'US')
    except Exception: return None


def main():
    if not args: sys.exit(__doc__)
    want = {'J': 'JP', 'U': 'US', 'E': 'EU'}.get((args[1:] or ['US'])[0].upper()[:1], 'US')
    for d, dirs, files in os.walk(os.path.expanduser(args[0]), followlinks=True):
        dirs.sort()
        for f in sorted(files):
            if not f.lower().endswith('.cue'): continue
            p = os.path.join(d, f)
            k = build_ui.identify(p)
            if not k or k[0] != 'shcd': continue
            r = region(p)
            if listing: print(r, os.path.abspath(p)); continue
            if r == want: print(os.path.abspath(p)); return


if __name__ == '__main__':
    main()
