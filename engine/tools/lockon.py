#!/usr/bin/env python3
"""lockon.py SANDK CART OUT [--shk2 PATCH]: a SHK lock-on image, as the cartridge
pair appears on the bus - SHK (2 MB) at $000000, the locked-on cartridge at $200000.

  SHK + SH1 -> "SHBS"             (2.5 MB)
  SHK + SH2 -> "SHK2"     (3.25 MB: + the 256 KB patch ROM at $300000)
  SHK + SH3 -> "SH3K"      (4 MB)

The SH2 patch ROM is a second chip on the SHK cartridge, absent from a plain SHK dump:
--shk2 takes it as a 256 KB file, or as a combined SHK + SH2 dump (its last 256 KB is used).
--extract-shk2 DUMP OUT writes that 256 KB from such a dump. Known-good SHA-1s are checked.
"""
import hashlib, sys

KNOWN = {   # No-Intro (World) lock-on dumps
    'a3084262f5af481df1a5c5ab03c4862551a53c91': 'SHK + SH1 (World)',
    '6cd0537a3aee0e012bb86d5837ddff9342595004': 'SHK + SH2 (World)',
    'cfbf98c36c776677290a872547ac47c53d2761d6': 'SHK + SH3 (World)',
}


def main():
    a = sys.argv[1:]
    if a and a[0] == '--extract-shk2':
        d = open(a[1], 'rb').read()
        if len(d) != 0x340000:
            sys.exit(f'{a[1]}: not a SHK + SH2 dump ({len(d):#x} bytes, expected 0x340000)')
        open(a[2], 'wb').write(d[0x300000:])
        print(f'{a[2]}: 256 KB patch ROM')
        return
    sk, cart = open(a[0], 'rb').read(), open(a[1], 'rb').read()
    if len(sk) != 0x200000 or sk[0x180:0x18E] != b'GM MK-1563 -00':     # SHK: its header serial
        sys.exit(f'{a[0]}: not a SHK ROM')
    img = sk + cart
    if cart[0x180:0x18E] in (b'GM 00001051-00', b'GM 00001051-01', b'GM 00001051-02'):   # SH2
        if '--shk2' not in a:
            sys.exit('SHK + SH2 needs --shk2 (the patch ROM, or a SHK + SH2 dump)')
        p = open(a[a.index('--shk2') + 1], 'rb').read()
        img += p[-0x40000:]
    open(a[2], 'wb').write(img)
    h = hashlib.sha1(img).hexdigest()
    print(f'{a[2]}: {len(img):#x} bytes, SHA-1 {h}' + (f' = {KNOWN[h]}' if h in KNOWN else ' (no known dump)'))


if __name__ == '__main__':
    main()
