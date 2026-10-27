"""nemesis.py - Sega's Nemesis tile compression (Mega Drive), decode and encode, our own
implementation from the format's description: header word (bit 15: XOR mode, low 15 bits: tile
count), a code table (palette index runs, each with a bit-length and code), the stream of codes
(a run of up to 8 pixels of one index), or an escape (6 bits: 111111 + 3-bit count + 4-bit index)."""


def decode(data, at=0):
    """-> (bytes of tiles, end offset)."""
    hdr = data[at] << 8 | data[at + 1]
    xor, ntiles = hdr & 0x8000, hdr & 0x7FFF
    p = at + 2
    table = {}                       # (length, code) -> (count, index)
    idx = 0
    while True:
        b = data[p]; p += 1
        if b == 0xFF:
            break
        if b & 0x80:
            idx = b & 0xF
            b = data[p]; p += 1
        cnt = ((b >> 4) & 7) + 1
        ln = b & 0xF
        code = data[p]; p += 1
        table[(ln, code)] = (cnt, idx)
    # bit reader
    pos = p * 8
    def bits(n):
        nonlocal pos
        v = 0
        for _ in range(n):
            v = (v << 1) | ((data[pos >> 3] >> (7 - (pos & 7))) & 1)
            pos += 1
        return v
    total = ntiles * 64
    pix = []
    while len(pix) < total:
        code, ln = 0, 0
        while True:
            code = (code << 1) | bits(1); ln += 1
            if ln == 6 and code == 0x3F:
                v = bits(7); cnt, ix = ((v >> 4) & 7) + 1, v & 0xF
                break
            if (ln, code) in table:
                cnt, ix = table[(ln, code)]
                break
            if ln > 8:
                raise ValueError('bad Nemesis code at bit %d' % pos)
        pix += [ix] * cnt
    pix = pix[:total]
    out = bytearray()
    prev = 0
    for row in range(len(pix) // 8):
        v = 0
        for x in range(8):
            v = (v << 4) | pix[row * 8 + x]
        if xor:
            v ^= prev; prev = v
        out += v.to_bytes(4, 'big')
    return bytes(out), (pos + 7) // 8


def encode(tiles):
    """Plain (non-XOR) Nemesis stream; every run coded with the escape-free table built from
    run frequencies (simple length-limited codes: runs sorted by frequency get 8-bit codes
    0x00.. unless rare -> escape)."""
    pix = []
    for b in tiles:
        pix += [b >> 4, b & 15]
    runs = []
    i = 0
    while i < len(pix):
        j = i
        while j < len(pix) and j - i < 8 and pix[j] == pix[i] and (j - i == 0 or (j // 8) == (i // 8)):
            j += 1
        runs.append((j - i, pix[i])); i = j
    import collections
    freq = collections.Counter(runs).most_common()
    # canonical-ish prefix codes: 7-bit codes for the 63 most common, avoiding prefix 111111
    table = {}
    codes = [c for c in range(128) if (c >> 1) != 0x3F][:62]
    for (run, c) in zip(freq, codes):
        table[run[0]] = (7, c)
    out = bytearray([(len(tiles) // 32) >> 8 & 0x7F, (len(tiles) // 32) & 0xFF])
    last = None
    for (cnt, ix), (ln, code) in sorted(table.items(), key=lambda kv: kv[0][1]):
        if ix != last:
            out.append(0x80 | ix); last = ix
        out.append(((cnt - 1) << 4) | ln); out.append(code)
    out.append(0xFF)
    bitsl = []
    for r in runs:
        if r in table:
            ln, code = table[r]
            bitsl += [(code >> (ln - 1 - k)) & 1 for k in range(ln)]
        else:
            v = 0x3F << 7 | (r[0] - 1) << 4 | r[1]
            bitsl += [(v >> (12 - k)) & 1 for k in range(13)]
    while len(bitsl) % 8:
        bitsl.append(0)
    for k in range(0, len(bitsl), 8):
        out.append(int(''.join(map(str, bitsl[k:k + 8])), 2))
    return bytes(out)
