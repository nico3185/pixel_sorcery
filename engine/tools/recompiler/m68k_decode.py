"""m68k_decode.py: one 68000 instruction, decoded as a 68000 executes it.

Capstone decodes for the whole 680x0 family; on 68000 bytes it differs from the CPU in three
ways that isa_check.py (engine/tools/isa) measured:
  - an index extension word ((d8,An,Xn), (d8,PC,Xn)) with bits 8-10 set reads as a 68020 scaled
    or memory-indirect operand (different address, different length); the 68000 ignores bits 8-10;
  - ANDI/ORI/EORI to CCR and BTST/BCHG/BCLR/BSET #n with a non-zero high byte in the immediate
    word are refused; the 68000 ignores that byte;
  - ABCD/SBCD -(Ay),-(Ax) is given a phantom immediate word (4 bytes instead of 2);
  - some register shifts/rotates get the wrong size and count (ROXR.b D3,D6 read as roxr.l #3,d6):
    those are decoded here from their (simple) encoding;
  - BTST Dn,#imm takes a 32-bit immediate (6 bytes); on the 68000 it is one word, low byte used.
normalize() clears what the 68000 ignores before capstone sees the bytes; decode() also fixes the
ABCD/SBCD length. Every caller (analyzer, generator, check_seeds, kpi) decodes through here.
"""
import re

_CCR_IMM = (0x003C, 0x023C, 0x0A3C)       # ORI/ANDI/EORI #,CCR


def _ea_words(mode, reg, size):
    """Extension words of an effective address (size: 1/2/4 bytes, for #imm)."""
    if mode in (5, 6):
        return 1
    if mode == 7:
        return {0: 1, 1: 2, 2: 1, 3: 1, 4: 2 if size == 4 else 1}.get(reg, 0)
    return 0


def _index_eas(op):
    """[(word offset of the EA's first extension word, mode, reg, size)] for the EAs of op whose
    extension words could be brief index words; [] when op has no such EA."""
    hi = op >> 12
    m, r = (op >> 3) & 7, op & 7
    if hi in (1, 2, 3):                                   # MOVE / MOVEA: source, then destination
        size = {1: 1, 3: 2, 2: 4}[hi]
        dm, dr = (op >> 6) & 7, (op >> 9) & 7
        return [(1, m, r, size), (1 + _ea_words(m, r, size), dm, dr, size)]
    if hi == 0:
        if op & 0x0100:                                   # BTST/BCHG/BCLR/BSET Dn,<ea>; MOVEP
            return [] if m == 1 else [(1, m, r, 1)]
        if (op & 0x0F00) == 0x0800:                       # bit op #imm,<ea>
            return [(2, m, r, 1)]
        if (op & 0x00FF) in (0x3C, 0x7C):                 # #,CCR / #,SR
            return []
        sz = (op >> 6) & 3
        if sz == 3:
            return []
        return [(2 if sz < 2 else 3, m, r, (1, 2, 4)[sz])]   # ORI/ANDI/SUBI/ADDI/EORI/CMPI
    if hi == 4:
        if 0x4E40 <= op <= 0x4E7F or op == 0x4AFC:        # TRAP, LINK, UNLK, MOVE USP, RTS...
            return []
        if (op & 0xFB80) == 0x4880 and m >= 2:            # MOVEM: register mask first
            return [(2, m, r, 2)]
        return [(1, m, r, 2)]
    if hi == 5:
        if (op & 0xF0F8) == 0x50C8:                       # DBcc
            return []
        return [(1, m, r, (1, 2, 4, 1)[(op >> 6) & 3])]
    if hi in (8, 9, 0xB, 0xC, 0xD):
        return [(1, m, r, 4 if (op >> 6) & 7 in (2, 6, 7) else 2)]
    if hi == 0xE and (op & 0xC0) == 0xC0:                 # memory shift/rotate
        return [(1, m, r, 2)]
    return []


def normalize(code):
    """code (bytes from the instruction's address) with the bits the 68000 ignores cleared."""
    if len(code) < 2:
        return code
    b = bytearray(code)
    op = b[0] << 8 | b[1]
    if (op in _CCR_IMM or (op & 0xFF00) == 0x0800) and len(b) >= 4:
        b[2] = 0                                           # immediate word: high byte ignored
    for k, mode, reg, _ in _index_eas(op):
        if (mode == 6 or (mode == 7 and reg == 3)) and 2 * k + 1 < len(b):
            b[2 * k] &= 0xF8                               # extension bits 8-10
    return bytes(b)


def decode(cs, code, addr):
    """(mnemonic, op_str, size, bytes) of the instruction at addr, or None."""
    d = decode_insn(cs, code, addr)
    return d and d[:4]


def decode_insn(cs, code, addr):
    """decode()'s tuple plus capstone's instruction object (operands, when cs.detail is on)."""
    code = normalize(code)
    ins = next(cs.disasm(code, addr, count=1), None)
    if ins is None:
        return None
    mnem, ops, size = ins.mnemonic.lower(), ins.op_str, ins.size
    op = code[0] << 8 | code[1]
    if (op & 0xF000) == 0xE000 and (op & 0xC0) != 0xC0:      # register shift/rotate
        kind = ('as', 'ls', 'rox', 'ro')[(op >> 3) & 3]
        cnt = f'd{(op >> 9) & 7}' if op & 0x20 else f'#${((op >> 9) & 7) or 8:x}'
        mnem = f"{kind}{'l' if op & 0x100 else 'r'}.{'bwl'[(op >> 6) & 3]}"
        ops, size = f'{cnt}, d{op & 7}', 2
    elif (op & 0xF1FF) == 0x013C and len(code) >= 4:          # BTST Dn,#imm
        mnem, ops, size = 'btst.b', f'd{(op >> 9) & 7}, #${code[3]:x}', 4
    if mnem.split('.')[0] in ('abcd', 'sbcd') and size == 4:
        size = 2
        ops = re.sub(r',\s*#\$?[0-9a-fx]+\s*$', '', ops)
    return mnem, ops, size, code[:size], ins
