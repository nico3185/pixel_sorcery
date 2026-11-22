#!/usr/bin/env python3
"""hide_symbols.py OBJCOPY NM OBJ ENTRY: in one title's partially linked object, keep only ENTRY global (GNU objcopy).

MinGW reaches data in another object through .refptr.NAME COMDATs: the ones for engine data the title only uses
(undefined here) stay global, or the final link finds .refptr.ps_frames etc. undefined. The title's own names stay
hidden, so two titles never meet."""
import subprocess, sys


def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


objcopy, nm, obj, entry = sys.argv[1:5]
undefined = {l.split()[-1] for l in run([nm, '-u', obj]).splitlines() if l.strip()}
defined = {l.split()[-1] for l in run([nm, '--defined-only', obj]).splitlines() if l.strip()}
keep = [entry] + sorted('.refptr.' + n for n in undefined if '.refptr.' + n in defined)
run([objcopy] + ['--keep-global-symbol=' + k for k in keep] + [obj])
