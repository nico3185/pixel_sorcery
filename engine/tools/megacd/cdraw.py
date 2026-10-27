"""cdraw.py: CD-ROM Mode 1 raw sectors (2352 bytes) from 2048-byte user data (ECMA-130):
sync, header (BCD minute/second/frame + mode 1), data, EDC (CRC-32, polynomial $8001801B,
reflected), 8 zero bytes, ECC P (86 x RS(26,24)) and Q (52 x RS(45,43)) parity over GF(2^8).
Emulators such as OpenEmu expect a disc's data track this way (.cue "MODE1/2352")."""

_edc = []
for i in range(256):
    e = i
    for _ in range(8):
        e = (e >> 1) ^ (0xD8018001 if e & 1 else 0)
    _edc.append(e)
_f = [0] * 256                                 # GF(2^8) log tables for the ECC
_b = [0] * 256
for i in range(256):
    j = (i << 1) ^ (0x11D if i & 0x80 else 0)
    _f[i] = j & 0xFF
    _b[i ^ j] = i


def edc(data):
    e = 0
    for x in data:
        e = (e >> 8) ^ _edc[(e ^ x) & 0xFF]
    return e


def _ecc(sec, major_count, minor_count, major_mult, minor_inc, dest):
    size = major_count * minor_count
    for major in range(major_count):
        index = (major >> 1) * major_mult + (major & 1)
        a = b = 0
        for _ in range(minor_count):
            t = sec[12 + index]
            index += minor_inc
            if index >= size:
                index -= size
            a ^= t
            b ^= t
            a = _f[a]
        a = _b[_f[a] ^ b]
        sec[dest + major] = a
        sec[dest + major + major_count] = a ^ b


def sector(lba, data):
    """2352 raw bytes for user data `data` (2048) at logical block `lba` (+150 frames)."""
    s = bytearray(2352)
    s[0:12] = b'\x00' + b'\xff' * 10 + b'\x00'
    m, rem = divmod(lba + 150, 75 * 60)
    sc, fr = divmod(rem, 75)
    bcd = lambda v: (v // 10) << 4 | v % 10
    s[12:16] = bytes([bcd(m), bcd(sc), bcd(fr), 1])
    s[16:2064] = data
    s[2064:2068] = edc(s[0:2064]).to_bytes(4, 'little')
    _ecc(s, 86, 24, 2, 86, 2076)                # P parity
    _ecc(s, 52, 43, 86, 88, 2248)               # Q parity
    return bytes(s)
