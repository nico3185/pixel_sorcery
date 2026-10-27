#!/usr/bin/env python3
"""build_disc.py SECURITY_DISC OUT.iso [--region E|U|J]: a bootable Mega CD image of our own.

SECURITY_DISC: any disc of that region you own (.cue or its data .bin, 2352-byte sectors, or a
2048-byte .iso). Its boot sector's security block - Sega's code the BIOS checks before it runs
the initial program - is copied into OUT at build time; nothing of it is stored in the repository.
The header, the main-CPU program (ip.s), the sub-CPU module (sp.s) and the payload are ours,
assembled with m68k-elf-as/ld (brew install m68k-elf-binutils). The payload (hello.s, or --payload
FILE.bin linked for $200000) is stored after the SP; the SP loads it into word RAM, the IP runs it.
"""
import os, subprocess, sys, tempfile

SEC_LEN = {'E': 0x56E, 'U': 0x584, 'J': 0x156}
here = os.path.dirname(os.path.abspath(__file__))


def read_data(path, n):
    if path.endswith('.cue'):
        line = next(l for l in open(path) if l.startswith('FILE'))
        path = os.path.join(os.path.dirname(path), line.split('"')[1])
    raw = open(path, 'rb').read(n * 2352 // 2048 + 4096)
    if raw[:12] == b'\x00' + b'\xff' * 10 + b'\x00':        # raw 2352-byte sectors
        return b''.join(raw[i * 2352 + 16:i * 2352 + 16 + 2048] for i in range(n // 2048))
    return raw[:n]


def assemble(src, tmp, org=0):
    o, b = os.path.join(tmp, src + '.o'), os.path.join(tmp, src + '.bin')
    subprocess.run(['m68k-elf-as', '-m68000', '-I', tmp, '-o', o, os.path.join(here, src)], check=True)
    subprocess.run(['m68k-elf-ld', f'-Ttext={org:#x}', '--oformat=binary', '-o', b, o], check=True)
    return open(b, 'rb').read()


def main():
    src, out = sys.argv[1], sys.argv[2]
    region = sys.argv[sys.argv.index('--region') + 1] if '--region' in sys.argv else 'E'
    boot = read_data(src, 0x800)
    if boot[:14] != b'SEGADISCSYSTEM':
        sys.exit(f'{src}: not a Mega CD data track')
    sec = boot[0x200:0x200 + SEC_LEN[region]]
    # --game N:PREFIX (repeatable; a bare PREFIX is game 1): relocate.py's PREFIX.trapcart.md -
    # word RAM ($200000-$23FFFF) and program RAM bank 1 (main $020000-$03FFFF). --menu ART:
    # menu_art.py's output, shown by menu.s; without it a single game starts directly.
    games = {}
    for i, a_ in enumerate(sys.argv):
        if a_ == '--game':
            v = sys.argv[i + 1]
            n_, pre = (int(v.split(':', 1)[0]), v.split(':', 1)[1]) if ':' in v else (1, v)
            games[n_] = pre
    menu_art = sys.argv[sys.argv.index('--menu') + 1] if '--menu' in sys.argv else None
    with tempfile.TemporaryDirectory() as tmp:
        open(os.path.join(tmp, 'swap.bin'), 'wb').write(assemble('swap.s', tmp, 0xFFFD18))   # ip.s includes it
        ip, sp = bytearray(assemble('ip.s', tmp)), bytearray(assemble('sp.s', tmp))
        hello = assemble('hello.s', tmp, 0x200000)
        menu = None
        if menu_art:
            art = open(menu_art, 'rb').read()
            cram = [int.from_bytes(art[i * 2:i * 2 + 2], 'big') for i in range(64)]
            rgb = lambda c: ((c >> 1) & 7, (c >> 5) & 7, (c >> 9) & 7)
            best = min((i for i in range(64) if i % 16), key=lambda i: sum((x - y) ** 2 for x, y in zip(rgb(cram[i]), (7, 7, 0))))
            import shutil
            shutil.copy(menu_art, os.path.join(tmp, 'menu_art.bin'))
            o, bn = os.path.join(tmp, 'menu.o'), os.path.join(tmp, 'menu.bin')
            subprocess.run(['m68k-elf-as', '-m68000', '-I', tmp, f'--defsym=YPAL={best // 16}', f'--defsym=YIDX={best % 16}',
                            '-o', o, os.path.join(here, 'menu.s')], check=True)
            subprocess.run(['m68k-elf-ld', '-Ttext=0x200000', '--oformat=binary', '-o', bn, o], check=True)
            menu = bytearray(open(bn, 'rb').read())
            av = menu.find(b'AVAIL!!!') + 8
            for n_ in games:
                menu[av + n_ - 1] = 1
    groups = [[] for _ in range(24)]              # group: [(bytes, sub-CPU destination)]
    entries = {}
    def image(cart):                              # word RAM, program-RAM bank 1, and banks 2/3:
        g = [(cart[0x200000:0x240000], 0x080000), (cart[0x020000:0x040000], 0x020000)]
        for virt, bank in ((0x0A0000, 0x040000), (0x0E0000, 0x060000)):   # mirrors of the window
            img = cart[virt:virt + 0x20000]                               # a game selects
            if img != b'\x4a\xfc' * 0x10000:                             # (relocate.py virtual
                g.append((img, bank))                                     # addresses)
        return g
    for n_, pre in sorted(games.items()):
        cart = open(pre + '.trapcart.md', 'rb').read()
        groups[n_] = image(cart)
        v = lambda i: int.from_bytes(cart[i * 4:i * 4 + 4], 'big')
        entries[n_] = v(0).to_bytes(4, 'big') + v(30).to_bytes(4, 'big') + v(28).to_bytes(4, 'big') + \
            v(26).to_bytes(4, 'big') + bytes([0, 0x40]) + v(1).to_bytes(4, 'big')
    for i, a_ in enumerate(sys.argv):             # --image G:PREFIX: another image of a game (a
        if a_ == '--image':                       # zone, the front end), loaded as group G when
            gi, pre = sys.argv[i + 1].split(':', 1)   # its swap.s thunk asks
            groups[int(gi)] = image(open(pre + '.trapcart.md', 'rb').read())
    if menu:
        groups[0], mode = [(bytes(menu), 0x080000)], 2
    elif 1 in games:
        groups[0], mode = groups[1], 1
    else:
        payload = open(sys.argv[sys.argv.index('--payload') + 1], 'rb').read() if '--payload' in sys.argv else hello
        if len(payload) > 0x40000:
            sys.exit(f'payload {len(payload)} bytes: word RAM holds 256 KB')
        groups[0], mode = [(payload, 0x080000)], 0
    # --scd CUE: a Mega CD game (SHCD) as game 8. Its data track is the base of this disc
    # (files where the game reads them; our boot area replaces its first sectors, its file system
    # from sector 16 stays) and its audio tracks follow in OUT.cue; its own boot area (sector 0 +
    # IP, its module) is group 8, chained to by sp.s/ip.s.
    base, audio, chain = b'', [], 0
    if '--scd' in sys.argv:
        cue = sys.argv[sys.argv.index('--scd') + 1]
        cdir = os.path.dirname(os.path.abspath(cue))
        lines = open(cue).read().splitlines()
        files = [(l.split('"')[1], i) for i, l in enumerate(lines) if l.startswith('FILE')]
        raw = open(os.path.join(cdir, files[0][0]), 'rb').read()
        base = b''.join(raw[k * 2352 + 16:k * 2352 + 16 + 2048] for k in range(len(raw) // 2352)) \
            if raw[:12] == b'\x00' + b'\xff' * 10 + b'\x00' else raw
        orig = base[:0x8000]
        groups[8] = [(orig[:0x1000], 0x080000), (orig[0x1000:0x8000], 0x040000)]
        entries[8] = b'SCD!' + bytes(18)
        chain = 0x88
        if menu:
            menu[menu.find(b'AVAIL!!!') + 8 + 7] = 1
        for fi, (fn, li) in enumerate(files[1:]):
            nxt = files[fi + 2][1] if fi + 2 < len(files) else len(lines)
            audio.append((os.path.join(cdir, fn), lines[li + 1:nxt]))
        if mode == 2:
            groups[0] = [(bytes(menu), 0x080000)]
    images = [im for g in groups for im in g]
    gamevec = entries or None
    ipsz, spsz = 0x800, (len(sp) + 0x7FF) & ~0x7FF
    head = max(len(base), 0x1000 + spsz)
    lba = (head + 2047) // 2048
    t = sp.find(b'LOADTBL!') + 8
    size = sum((len(b_) + 2047) // 2048 * 2048 for b_, _ in images)
    disc = bytearray(lba * 2048 + size)
    disc[:len(base)] = base
    c_ = sp.find(b'CHAINON!') + 8
    sp[c_] = chain
    placed = {}
    for gi, g in enumerate(groups):
        for k, (img, dest) in enumerate(g):
            key = (id(img), dest)
            if key not in placed:                 # group 0 may reuse game 1's images
                count = (len(img) + 2047) // 2048
                disc[lba * 2048:lba * 2048 + len(img)] = img
                placed[key] = (lba, count)
                lba += count
            l_, c_ = placed[key]
            o_ = t + gi * (6 * 12 + 4) + 12 * k
            sp[o_:o_ + 12] = l_.to_bytes(4, 'big') + c_.to_bytes(4, 'big') + dest.to_bytes(4, 'big')
    g_ = ip.find(b'GAMEVEC!') + 8
    ip[g_] = mode
    for n_, e in entries.items():
        o_ = g_ + 2 + (n_ - 1) * 22
        ip[o_:o_ + 22] = e
    disc = disc[:lba * 2048]
    payload = b''.join(i for i, _ in images)
    print(f'groups: ' + ', '.join(f'{i}:{len(g)}' for i, g in enumerate(groups) if g) + f'; mode {mode}')
    h = disc
    h[0x00:0x10] = b'SEGADISCSYSTEM  '
    h[0x10:0x1B] = b'PIXELSORC  '; h[0x1C:0x20] = (0x0100_0001).to_bytes(4, 'big')
    h[0x20:0x2B] = b'PIXELSORC  '; h[0x2C:0x30] = (0x0001_0000).to_bytes(4, 'big')
    h[0x30:0x38] = (0x800).to_bytes(4, 'big') + ipsz.to_bytes(4, 'big')        # IP: offset, size
    h[0x40:0x48] = (0x1000).to_bytes(4, 'big') + spsz.to_bytes(4, 'big')       # SP
    h[0x100:0x200] = b' ' * 0x100
    h[0x100:0x110] = b'SEGA MEGA DRIVE '
    h[0x120:0x130] = b'PIXELSORCERY    '
    h[0x180:0x18E] = b'GM 00000000-00'
    h[0x1F0] = ord(region)
    h[0x200:0x200 + len(sec)] = sec                                            # security block
    code = 0x200 + len(sec)
    if code + len(ip) > 0x1000:
        sys.exit('IP too large')
    h[code:code + len(ip)] = ip
    h[0x1000:0x1000 + len(sp)] = sp
    open(out, 'wb').write(bytes(disc) + bytes(2048 * 16))                       # padded
    if audio:                                                                    # OUT.cue: our data
        odir = os.path.dirname(os.path.abspath(out))                             # track, its audio
        sys.path.insert(0, here)                 # the data track as raw Mode 1 sectors (OUT.bin):
        import cdraw                             # what OpenEmu and most emulators expect
        rawp = os.path.splitext(out)[0] + '.bin'
        full = bytes(disc) + bytes(2048 * 16)
        with open(rawp, 'wb') as fo:
            for k in range(len(full) // 2048):
                fo.write(cdraw.sector(k, full[k * 2048:(k + 1) * 2048]))
        cue = [f'FILE "{os.path.basename(rawp)}" BINARY', '  TRACK 01 MODE1/2352', '    INDEX 01 00:00:00']
        for path, rest in audio:
            link = os.path.join(odir, os.path.basename(path))
            if os.path.islink(link):
                os.remove(link)
            if not os.path.exists(link):          # a real file (importers do not follow links)
                try:
                    os.link(path, link)
                except OSError:
                    import shutil
                    shutil.copyfile(path, link)
            cue.append(f'FILE "{os.path.basename(path)}" BINARY')
            cue += rest
        open(os.path.splitext(out)[0] + '.cue', 'w').write('\n'.join(cue) + '\n')
        print(f'{os.path.splitext(out)[0]}.cue: data track + {len(audio)} audio tracks (linked)')
    print(f'{out}: region {region}, security {len(sec)} bytes, IP {len(ip)} bytes at ${code:X}, SP {len(sp)} bytes, '
          f'{len(images)} image(s), {len(payload)} bytes' + (' (game)' if gamevec else ''))


if __name__ == '__main__':
    main()
