#!/usr/bin/env python3
"""
PixelSorcery Z80 static recompiler.

Translates a Z80 sound-driver image (the bytes a Genesis game uploads into Z80 RAM)
into one C function. Every address in the code ranges becomes a C label holding the
translated instruction that starts there, so any entry point (calls, returns, computed
jumps, interrupt vector, even entries into the middle of an instruction stream) is
reachable without an interpreter. Each label is also a resumable point: the generated
function runs until its master-cycle budget is spent or an interrupt is due, saves the
PC and returns; the next call re-enters through a switch on the saved PC.

Usage:
  z80recomp.py --image driver.bin --code 0x0000-0x0EEF [--code A-B ...]
               --name coi -o z80_coi.c

Generated code calls the runtime in engine/include/genrecomp/ps_z80rt.h.
"""
import argparse
import sys

R8 = ['b', 'c', 'd', 'e', 'h', 'l', None, 'a']          # index 6 = (HL)
RP = ['BC', 'DE', 'HL', 'SP']
RP2 = ['BC', 'DE', 'HL', 'AF']
CC = ['!(z->f & FZ)', '(z->f & FZ)', '!(z->f & FC)', '(z->f & FC)',
      '!(z->f & FP)', '(z->f & FP)', '!(z->f & FS)', '(z->f & FS)']
CCN = ['NZ', 'Z', 'NC', 'C', 'PO', 'PE', 'P', 'M']
ALU = ['ADD', 'ADC', 'SUB', 'SBC', 'AND', 'XOR', 'OR', 'CP']
ROT = ['RLC', 'RRC', 'RL', 'RR', 'SLA', 'SRA', 'SLL', 'SRL']


class Instr:
    def __init__(self):
        self.length = 1
        self.text = ''
        self.code = []          # C statements
        self.cycles = 4         # T-states (base / not-taken)
        self.end = False        # True if control never falls through


def s8(v):
    return v - 256 if v >= 128 else v


def pair_get(name, idx=None):
    """C expression for a 16-bit register pair; idx 'ix'/'iy' replaces HL."""
    if name == 'HL' and idx:
        return f'GET_{idx.upper()}(z)'
    return f'GET_{name}(z)'


def pair_set(name, val, idx=None):
    if name == 'HL' and idx:
        return f'SET_{idx.upper()}(z, {val});'
    return f'SET_{name}(z, {val});'


class Decoder:
    def __init__(self, mem):
        self.mem = mem

    def byte(self, a):
        return self.mem[a & 0xFFFF]

    def word(self, a):
        return self.byte(a) | (self.byte(a + 1) << 8)

    # ---- 8-bit register access under an index prefix ------------------------------
    def r_get(self, i, idx, addr_expr):
        if i == 6:
            return f'RD({addr_expr})'
        r = R8[i]
        if idx and r in ('h', 'l'):
            return f'z->{idx}{r}'
        return f'z->{r}'

    def r_set(self, i, idx, addr_expr, val):
        if i == 6:
            return f'WR({addr_expr}, {val});'
        r = R8[i]
        if idx and r in ('h', 'l'):
            return f'z->{idx}{r} = (uint8_t)({val});'
        return f'z->{r} = (uint8_t)({val});'

    def decode(self, pc):
        ins = Instr()
        op = self.byte(pc)
        if op in (0xDD, 0xFD):
            nxt = self.byte(pc + 1)
            idx = 'ix' if op == 0xDD else 'iy'
            if nxt in (0xDD, 0xFD, 0xED):
                ins.length = 1
                ins.text = f'DB ${op:02X} (prefix ignored)'
                ins.cycles = 4
                return ins
            if nxt == 0xCB:
                return self.decode_xcb(pc, idx)
            sub = self.decode_main(pc + 1, nxt, idx)
            sub.length += 1
            sub.cycles += 4
            # undocumented IXH/IXL/IYH/IYL register forms take one T-state more than the
            # prefix + base (9, not 8), as the reference core counts them
            x, y, z_ = nxt >> 6, (nxt >> 3) & 7, nxt & 7
            mem = (x == 1 and (y == 6 or z_ == 6)) or (x == 2 and z_ == 6)
            if not mem and ((x == 1 and (y in (4, 5) or z_ in (4, 5))) or (x == 2 and z_ in (4, 5))
                            or (x == 0 and z_ in (4, 5) and y in (4, 5))):
                sub.cycles += 1
            return sub
        if op == 0xCB:
            return self.decode_cb(pc)
        if op == 0xED:
            return self.decode_ed(pc)
        return self.decode_main(pc, op, None)

    # ---- unprefixed (and DD/FD-prefixed) opcodes ---------------------------------
    def decode_main(self, pc, op, idx):
        ins = Instr()
        x, y, z_, p, q = op >> 6, (op >> 3) & 7, op & 7, (op >> 4) & 3, (op >> 3) & 1
        HL = pair_get('HL', idx)
        # (HL) or (IX+d)
        uses_mem = (x == 1 and (y == 6 or z_ == 6) and not (y == 6 and z_ == 6)) or \
                   (x == 2 and z_ == 6) or (x == 0 and z_ in (4, 5, 6) and y == 6)
        disp = 0
        extra = 0
        if idx and uses_mem:
            disp = s8(self.byte(pc + 1))
            extra = 1
            maddr = f'(uint16_t)({HL} + ({disp}))'
        else:
            maddr = HL
        base = pc + 1 + extra          # operand position after opcode (+disp)
        nn = self.word(base)
        n = self.byte(base)
        ins.length = 1 + extra
        # within an (IX+d) instruction, H/L name the real H/L
        ridx = None if (idx and uses_mem) else idx
        c = ins.code

        def rn(i):
            return R8[i].upper() if i != 6 else (f'({idx.upper()}{disp:+d})' if idx and uses_mem else '(HL)')

        if x == 0:
            if z_ == 0:
                if y == 0:
                    ins.text = 'NOP'
                elif y == 1:
                    ins.text = "EX AF,AF'"
                    c.append('EX_AF(z);')
                elif y == 2:
                    t = (pc + 2 + s8(n)) & 0xFFFF
                    ins.length = 2; ins.cycles = 8
                    ins.text = f'DJNZ ${t:04X}'
                    c.append(f'if (--z->b) {{ CYC(5); JUMP(0x{t:04X}); }}')
                elif y == 3:
                    t = (pc + 2 + s8(n)) & 0xFFFF
                    ins.length = 2; ins.cycles = 12; ins.end = True
                    ins.text = f'JR ${t:04X}'
                    c.append(f'JUMP(0x{t:04X});')
                else:
                    t = (pc + 2 + s8(n)) & 0xFFFF
                    ins.length = 2; ins.cycles = 7
                    ins.text = f'JR {CCN[y-4]},${t:04X}'
                    c.append(f'if ({CC[y-4]}) {{ CYC(5); JUMP(0x{t:04X}); }}')
            elif z_ == 1:
                if q == 0:
                    ins.length = 3; ins.cycles = 10
                    ins.text = f'LD {RP[p] if not (p==2 and idx) else idx.upper()},${nn:04X}'
                    c.append(pair_set(RP[p], f'0x{nn:04X}', idx))
                else:
                    ins.cycles = 11
                    src = pair_get(RP[p], idx)
                    ins.text = f'ADD {"HL" if not idx else idx.upper()},{RP[p] if not (p==2 and idx) else idx.upper()}'
                    c.append(f'{{ uint16_t v = {src}; uint16_t r = ADD16(z, {HL}, v); {pair_set("HL", "r", idx)} }}')
            elif z_ == 2:
                if q == 0:
                    if p == 0:
                        ins.cycles = 7; ins.text = 'LD (BC),A'; c.append('WR(GET_BC(z), z->a);')
                    elif p == 1:
                        ins.cycles = 7; ins.text = 'LD (DE),A'; c.append('WR(GET_DE(z), z->a);')
                    elif p == 2:
                        ins.length = 3; ins.cycles = 16; ins.text = f'LD (${nn:04X}),HL'
                        c.append(f'{{ uint16_t v = {HL}; WR(0x{nn:04X}, v & 0xFF); WR(0x{(nn+1)&0xFFFF:04X}, v >> 8); }}')
                    else:
                        ins.length = 3; ins.cycles = 13; ins.text = f'LD (${nn:04X}),A'
                        c.append(f'WR(0x{nn:04X}, z->a);')
                else:
                    if p == 0:
                        ins.cycles = 7; ins.text = 'LD A,(BC)'; c.append('z->a = RD(GET_BC(z));')
                    elif p == 1:
                        ins.cycles = 7; ins.text = 'LD A,(DE)'; c.append('z->a = RD(GET_DE(z));')
                    elif p == 2:
                        ins.length = 3; ins.cycles = 16; ins.text = f'LD HL,(${nn:04X})'
                        c.append(f'{{ uint16_t v = RD(0x{nn:04X}) | (RD(0x{(nn+1)&0xFFFF:04X}) << 8); {pair_set("HL", "v", idx)} }}')
                    else:
                        ins.length = 3; ins.cycles = 13; ins.text = f'LD A,(${nn:04X})'
                        c.append(f'z->a = RD(0x{nn:04X});')
            elif z_ == 3:
                ins.cycles = 6
                g, s = pair_get(RP[p], idx), RP[p]
                ins.text = ('INC ' if q == 0 else 'DEC ') + RP[p]
                c.append(pair_set(s, f'(uint16_t)({g} {"+" if q == 0 else "-"} 1)', idx))
            elif z_ in (4, 5):
                ins.cycles = 11 if y == 6 else 4
                if idx and y == 6:
                    ins.cycles = 19; ins.length = 2
                fn = 'INC8' if z_ == 4 else 'DEC8'
                ins.text = ('INC ' if z_ == 4 else 'DEC ') + rn(y)
                if y == 6:
                    c.append(f'{{ uint16_t ad = {maddr}; WR(ad, {fn}(z, RD(ad))); }}')
                else:
                    c.append(self.r_set(y, ridx, maddr, f'{fn}(z, {self.r_get(y, ridx, maddr)})'))
            elif z_ == 6:
                ins.length = 2 + extra
                ins.cycles = 10 if y == 6 else 7
                if idx and y == 6:
                    ins.cycles = 15
                ins.text = f'LD {rn(y)},${n:02X}'
                c.append(self.r_set(y, ridx, maddr, f'0x{n:02X}'))
            else:  # z_ == 7
                ins.text = ['RLCA', 'RRCA', 'RLA', 'RRA', 'DAA', 'CPL', 'SCF', 'CCF'][y]
                c.append(['RLCA(z);', 'RRCA(z);', 'RLA(z);', 'RRA(z);', 'DAA(z);',
                          'CPL(z);', 'SCF(z);', 'CCF(z);'][y])
        elif x == 1:
            if y == 6 and z_ == 6:
                ins.text = 'HALT'
                c.append(f'HALT(0x{(pc + 1) & 0xFFFF:04X});')
                ins.end = True
            else:
                ins.cycles = 7 if (y == 6 or z_ == 6) else 4
                if idx and (y == 6 or z_ == 6):
                    ins.cycles = 15
                ins.text = f'LD {rn(y)},{rn(z_)}'
                c.append(self.r_set(y, ridx, maddr, self.r_get(z_, ridx, maddr)))
        elif x == 2:
            ins.cycles = 7 if z_ == 6 else 4
            if idx and z_ == 6:
                ins.cycles = 15
            ins.text = f'{ALU[y]} {rn(z_)}'
            c.append(f'{ALU[y]}8(z, {self.r_get(z_, ridx, maddr)});')
        else:  # x == 3
            if z_ == 0:
                ins.cycles = 5
                ins.text = f'RET {CCN[y]}'
                c.append(f'if ({CC[y]}) {{ CYC(6); RET(); }}')
            elif z_ == 1:
                if q == 0:
                    ins.cycles = 10
                    nm = RP2[p] if not (p == 2 and idx) else idx.upper()
                    ins.text = f'POP {nm}'
                    c.append(f'{{ uint16_t v = POP16(z); {pair_set(RP2[p], "v", idx)} }}')
                else:
                    if p == 0:
                        ins.cycles = 10; ins.text = 'RET'; ins.end = True; c.append('RET();')
                    elif p == 1:
                        ins.text = 'EXX'; c.append('EXX(z);')
                    elif p == 2:
                        ins.text = f'JP ({"HL" if not idx else idx.upper()})'; ins.end = True
                        c.append(f'JUMPX({HL});')
                    else:
                        ins.cycles = 6; ins.text = 'LD SP,HL'; c.append(f'z->sp = {HL};')
            elif z_ == 2:
                ins.length = 3; ins.cycles = 10
                ins.text = f'JP {CCN[y]},${nn:04X}'
                c.append(f'if ({CC[y]}) {{ JUMP(0x{nn:04X}); }}')
            elif z_ == 3:
                if y == 0:
                    ins.length = 3; ins.cycles = 10; ins.end = True
                    ins.text = f'JP ${nn:04X}'
                    c.append(f'JUMP(0x{nn:04X});')
                elif y == 2:
                    ins.length = 2; ins.cycles = 11; ins.text = f'OUT (${n:02X}),A'
                    c.append(f'OUT(0x{n:02X}, z->a);')
                elif y == 3:
                    ins.length = 2; ins.cycles = 11; ins.text = f'IN A,(${n:02X})'
                    c.append(f'z->a = IN(0x{n:02X});')
                elif y == 4:
                    ins.cycles = 19; ins.text = 'EX (SP),HL'
                    c.append(f'{{ uint16_t v = RD(z->sp) | (RD((uint16_t)(z->sp + 1)) << 8); uint16_t h = {HL}; '
                             f'WR(z->sp, h & 0xFF); WR((uint16_t)(z->sp + 1), h >> 8); {pair_set("HL", "v", idx)} }}')
                elif y == 5:
                    ins.text = 'EX DE,HL'; c.append('EX_DEHL(z);')
                elif y == 6:
                    ins.text = 'DI'; c.append('z->iff1 = z->iff2 = 0;')
                else:
                    ins.text = 'EI'; c.append('z->iff1 = z->iff2 = 1; z->ei_block = 1;')
            elif z_ == 4:
                ins.length = 3; ins.cycles = 10
                ins.text = f'CALL {CCN[y]},${nn:04X}'
                c.append(f'if ({CC[y]}) {{ CYC(7); PUSH16(z, 0x{(pc+3)&0xFFFF:04X}); JUMP(0x{nn:04X}); }}')
            elif z_ == 5:
                if q == 0:
                    ins.cycles = 11
                    nm = RP2[p] if not (p == 2 and idx) else idx.upper()
                    ins.text = f'PUSH {nm}'
                    c.append(f'PUSH16(z, {pair_get(RP2[p], idx)});')
                else:  # p == 0 -> CALL nn (others are prefixes handled earlier)
                    ins.length = 3; ins.cycles = 17; ins.end = True
                    ins.text = f'CALL ${nn:04X}'
                    c.append(f'PUSH16(z, 0x{(pc+3)&0xFFFF:04X}); JUMP(0x{nn:04X});')
            elif z_ == 6:
                ins.length = 2; ins.cycles = 7
                ins.text = f'{ALU[y]} ${n:02X}'
                c.append(f'{ALU[y]}8(z, 0x{n:02X});')
            else:
                ins.cycles = 11; ins.end = True
                ins.text = f'RST ${y*8:02X}'
                c.append(f'PUSH16(z, 0x{(pc+1)&0xFFFF:04X}); JUMP(0x{y*8:04X});')
        if ins.end and pc_needs_next(ins):
            pass
        return ins

    # ---- CB prefix ----------------------------------------------------------------
    def decode_cb(self, pc):
        ins = Instr()
        op = self.byte(pc + 1)
        x, y, z_ = op >> 6, (op >> 3) & 7, op & 7
        ins.length = 2
        ins.cycles = 15 if z_ == 6 else 8
        rname = '(HL)' if z_ == 6 else R8[z_].upper()
        get = 'RD(GET_HL(z))' if z_ == 6 else f'z->{R8[z_]}'
        c = ins.code
        if x == 0:
            ins.text = f'{ROT[y]} {rname}'
            if z_ == 6:
                c.append(f'{{ uint16_t ad = GET_HL(z); WR(ad, {ROT[y]}(z, RD(ad))); }}')
            else:
                c.append(f'z->{R8[z_]} = {ROT[y]}(z, z->{R8[z_]});')
        elif x == 1:
            ins.cycles = 12 if z_ == 6 else 8
            ins.text = f'BIT {y},{rname}'
            if z_ == 6:
                c.append(f'BITM(z, {y}, RD(GET_HL(z)), (uint8_t)(GET_HL(z) >> 8));')
            else:
                c.append(f'BIT(z, {y}, z->{R8[z_]});')
        else:
            m = f'(uint8_t)~0x{1 << y:02X}' if x == 2 else f'0x{1 << y:02X}'
            opr = '&' if x == 2 else '|'
            ins.text = f'{"RES" if x == 2 else "SET"} {y},{rname}'
            if z_ == 6:
                c.append(f'{{ uint16_t ad = GET_HL(z); WR(ad, RD(ad) {opr} {m}); }}')
            else:
                c.append(f'z->{R8[z_]} {opr}= {m};')
        return ins

    # ---- DD CB / FD CB ------------------------------------------------------------
    def decode_xcb(self, pc, idx):
        ins = Instr()
        d = s8(self.byte(pc + 2))
        op = self.byte(pc + 3)
        x, y, z_ = op >> 6, (op >> 3) & 7, op & 7
        ins.length = 4
        ins.cycles = 23
        ad = f'(uint16_t)(GET_{idx.upper()}(z) + ({d}))'
        c = ins.code
        copy = f' z->{R8[z_]} = v;' if z_ != 6 else ''
        if x == 0:
            ins.text = f'{ROT[y]} ({idx.upper()}{d:+d})'
            c.append(f'{{ uint16_t ad = {ad}; uint8_t v = {ROT[y]}(z, RD(ad)); WR(ad, v);{copy} }}')
        elif x == 1:
            ins.cycles = 20
            ins.text = f'BIT {y},({idx.upper()}{d:+d})'
            c.append(f'{{ uint16_t ad = {ad}; BITM(z, {y}, RD(ad), (uint8_t)(ad >> 8)); }}')
        else:
            m = f'(uint8_t)~0x{1 << y:02X}' if x == 2 else f'0x{1 << y:02X}'
            opr = '&' if x == 2 else '|'
            ins.text = f'{"RES" if x == 2 else "SET"} {y},({idx.upper()}{d:+d})'
            c.append(f'{{ uint16_t ad = {ad}; uint8_t v = RD(ad) {opr} {m}; WR(ad, v);{copy} }}')
        return ins

    # ---- ED prefix ----------------------------------------------------------------
    def decode_ed(self, pc):
        ins = Instr()
        op = self.byte(pc + 1)
        x, y, z_, p, q = op >> 6, (op >> 3) & 7, op & 7, (op >> 4) & 3, (op >> 3) & 1
        ins.length = 2
        ins.cycles = 8
        nn = self.word(pc + 2)
        c = ins.code
        if x == 1:
            if z_ == 0:
                ins.cycles = 12
                if y == 6:
                    ins.text = 'IN (C)'; c.append('INF(z, IN(z->c));')
                else:
                    ins.text = f'IN {R8[y].upper()},(C)'
                    c.append(f'z->{R8[y]} = INF(z, IN(z->c));')
            elif z_ == 1:
                ins.cycles = 12
                v = '0' if y == 6 else f'z->{R8[y]}'
                ins.text = f'OUT (C),{"0" if y == 6 else R8[y].upper()}'
                c.append(f'OUT(z->c, {v});')
            elif z_ == 2:
                ins.cycles = 15
                fn = 'SBC16' if q == 0 else 'ADC16'
                ins.text = f'{fn[:3]} HL,{RP[p]}'
                c.append(f'SET_HL(z, {fn}(z, GET_HL(z), {pair_get(RP[p])}));')
            elif z_ == 3:
                ins.length = 4; ins.cycles = 20
                if q == 0:
                    ins.text = f'LD (${nn:04X}),{RP[p]}'
                    c.append(f'{{ uint16_t v = {pair_get(RP[p])}; WR(0x{nn:04X}, v & 0xFF); WR(0x{(nn+1)&0xFFFF:04X}, v >> 8); }}')
                else:
                    ins.text = f'LD {RP[p]},(${nn:04X})'
                    c.append(f'{{ uint16_t v = RD(0x{nn:04X}) | (RD(0x{(nn+1)&0xFFFF:04X}) << 8); {pair_set(RP[p], "v")} }}')
            elif z_ == 4:
                ins.text = 'NEG'; c.append('NEG(z);')
            elif z_ == 5:
                ins.cycles = 14; ins.end = True
                ins.text = 'RETI' if y == 1 else 'RETN'
                c.append('z->iff1 = z->iff2; RET();')
            elif z_ == 6:
                ins.text = f'IM {[0,0,1,2,0,0,1,2][y]}'
                c.append(f'z->im = {[0,0,1,2,0,0,1,2][y]};')
            else:
                ins.cycles = 9
                if y == 0:
                    ins.text = 'LD I,A'; c.append('z->i = z->a;')
                elif y == 1:
                    ins.text = 'LD R,A'; c.append('z->r = z->a;')
                elif y == 2:
                    ins.text = 'LD A,I'; c.append('z->a = z->i; LDAIR(z);')
                elif y == 3:
                    ins.text = 'LD A,R'; c.append('z->a = (uint8_t)((z->r & 0x80) | ((z->r + (z->cyc / 60)) & 0x7F)); LDAIR(z);')
                elif y == 4:
                    ins.cycles = 18; ins.text = 'RRD'; c.append('RRD(z);')
                elif y == 5:
                    ins.cycles = 18; ins.text = 'RLD'; c.append('RLD(z);')
                else:
                    ins.cycles = 8; ins.text = 'NOP (ED)'
        elif x == 2 and z_ <= 3 and y >= 4:
            names = [['LDI', 'CPI', 'INI', 'OUTI'], ['LDD', 'CPD', 'IND', 'OUTD'],
                     ['LDIR', 'CPIR', 'INIR', 'OTIR'], ['LDDR', 'CPDR', 'INDR', 'OTDR']]
            nm = names[y - 4][z_]
            ins.text = nm
            ins.cycles = 16
            step = nm[:3] if nm not in ('OUTI', 'OUTD', 'OTIR', 'OTDR') else 'OUT'
            dirn = '+1' if y in (4, 6) else '-1'
            base = {0: 'LDX', 1: 'CPX', 2: 'INX', 3: 'OUTX'}[z_]
            c.append(f'{base}(z, {dirn});')
            if y >= 6:
                cond = {0: 'GET_BC(z) != 0', 1: '(GET_BC(z) != 0) && !(z->f & FZ)',
                        2: 'z->b != 0', 3: 'z->b != 0'}[z_]
                c.append(f'if ({cond}) {{ CYC(5); JUMP(0x{pc:04X}); }}')
        else:
            ins.text = f'NOP (ED {op:02X})'
        return ins


def pc_needs_next(ins):
    return False


def parse_range(s):
    a, b = s.split('-')
    return int(a, 0), int(b, 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--image', required=True, help='Z80 RAM image (8 KB, address 0)')
    ap.add_argument('--code', action='append', required=True, help='code range A-B (inclusive)')
    ap.add_argument('--name', default='driver')
    ap.add_argument('--listing', help='write a disassembly listing here')
    ap.add_argument('-o', '--output', required=True)
    args = ap.parse_args()

    img = open(args.image, 'rb').read()
    mem = bytearray(0x10000)
    mem[0:len(img)] = img
    ranges = [parse_range(r) for r in args.code]
    dec = Decoder(mem)

    addrs = []
    for a, b in ranges:
        addrs.extend(range(a, b + 1))
    addrset = set(addrs)
    out = []
    W = out.append
    W(f'/* AUTO-GENERATED by engine/tools/z80recomp/z80recomp.py -- do not edit.')
    W(f' * Statically recompiled Z80 sound driver "{args.name}".')
    W(f' * Code ranges: {", ".join(f"${a:04X}-${b:04X}" for a, b in ranges)}.')
    W(f' * Every address is a label (resumable entry); see the generator for the model. */')
    W('#include "genrecomp/ps_z80rt.h"')
    W('')
    W(f'void ps_z80_{args.name}_run(PsZ80 *z)')
    W('{')
    W('    if (z->halted) goto halted;')
    W('    goto dispatch;')
    listing = []
    import re
    jre = re.compile(r'JUMP\(0x([0-9A-F]{4})\)')

    def resolve(stmt):
        def rep(m):
            t = int(m.group(1), 16)
            return f'goto L_{t:04X}' if t in addrset else f'JUMPX(0x{t:04X})'
        return jre.sub(rep, stmt)

    # Self-modifying code (SH2: jump tables made by patching a JR displacement, operands
    # and addresses rewritten before use): bytes the driver stores to with LD (nn),r inside
    # its code. An instruction holding one such byte becomes a switch on that byte's value in
    # RAM, one translation per value; a patched 16-bit immediate (LD rr,nn) is read from RAM.
    patched = set()
    pre = re.compile(r'LD \(\$([0-9A-F]{4})\),(\w+)$')
    for a in sorted(addrset):
        m = pre.match(dec.decode(a).text)
        if m:
            w = int(m.group(1), 16)
            for k in range(1 if m.group(2) == 'A' else 2):
                if (w + k) & 0xFFFF in addrset:
                    patched.add((w + k) & 0xFFFF)

    def emit(ins, a, ind):
        for s in ins.code:
            W(ind + resolve(s))
        nxt = (a + ins.length) & 0xFFFF
        if not ins.end:
            W(ind + (f'goto L_{nxt:04X};' if nxt in addrset else f'JUMPX(0x{nxt:04X});'))

    nvar = 0
    for a in sorted(addrset):
        ins = dec.decode(a)
        listing.append(f'{a:04X}  {ins.text}')
        hit = [b for b in range(a + 1, a + ins.length) if b in patched]
        W(f'L_{a:04X}: CHK(0x{a:04X});  /* {ins.text}{"  (patched)" if hit else ""} */')
        W(f'    CYC({ins.cycles});')
        if len(hit) == 2 and hit == [a + ins.length - 2, a + ins.length - 1]:
            # a patched 16-bit operand (immediate, address or jump target) read from RAM
            nn = f'0x{mem[hit[0]] | mem[hit[1]] << 8:04X}'
            dyn = f'(uint16_t)(z->ram[0x{hit[0]:04X}] | z->ram[0x{hit[1]:04X}] << 8)'
            ins.code = [c.replace(f'JUMP({nn})', f'JUMPX({dyn})').replace(nn, dyn) for c in ins.code]
            emit(ins, a, '    '); nvar += 1
        elif len(hit) == 1:
            b = hit[0]; keep = mem[b]
            W(f'    switch (z->ram[0x{b:04X}]) {{')
            for v in range(256):
                mem[b] = v
                vi = dec.decode(a)
                W(f'    case 0x{v:02X}: {{  /* {vi.text} */')
                if vi.cycles != ins.cycles:
                    W(f'        CYC({vi.cycles - ins.cycles});')
                emit(vi, a, '        ')
                W('    }')
            mem[b] = keep
            W('    }')
            nvar += 1
        else:
            if hit:
                print(f'z80recomp: ${a:04X} {ins.text}: patched bytes {hit} not handled', file=sys.stderr)
            emit(ins, a, '    ')
    W('dispatch:')
    W('    switch (z->pc) {')
    for a in sorted(addrset):
        W(f'    case 0x{a:04X}: goto L_{a:04X};')
    W('    default: ps_z80_miss(z); return;')
    W('    }')
    W('take_int:')
    W('    TAKE_INT();')
    W('    goto dispatch;')
    W('halted:')
    W('    if (z->irq_line && z->iff1) goto take_int;')
    W('    if (z->cyc < z->cyc_end) z->cyc = z->cyc_end;')
    W('    return;')
    W('}')
    open(args.output, 'w').write('\n'.join(out) + '\n')
    if args.listing:
        open(args.listing, 'w').write('\n'.join(listing) + '\n')
    print(f'z80recomp: {len(addrset)} addresses ({nvar} self-modified) -> {args.output}')


if __name__ == '__main__':
    main()
