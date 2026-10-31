#!/usr/bin/env python3
"""
genrecomp 68K recompiler: turns a Genesis ROM into C source for genrecomp.

One C function per discovered entry point, every instruction a statement on
g_m68k and the bus, calls through func_table_call and jumps out through
func_table_tail. Design and the traps it avoids: docs/recompiler.md.

Usage:
    python generate.py <rom> -o src/recomp [--config recomp.json]

The output is derived from the ROM. It belongs in the title's gitignored
build tree, never in a repository: each user generates it from their own ROM.
"""

import struct
import sys
import os
import re
from collections import defaultdict
from capstone import Cs, CS_ARCH_M68K, CS_MODE_M68K_000
from capstone.m68k import *

# Import the analyzer
sys.path.insert(0, os.path.dirname(__file__))
from analyze import GenesisROM, M68KAnalyzer


# ============================================================
# M68K → C instruction translator
# ============================================================


def _abs_addr(m):
    """Absolute EA value: `.w` (absolute short) is sign-extended to 32 bits,
    so $8000-$FFFF.w addresses work RAM at $FFFF8000-$FFFFFFFF."""
    v = int(m.group(1), 16)
    if (m.group(2) or '').lower() == 'w' and 0x8000 <= v <= 0xFFFF:
        v |= 0xFFFF0000
    return v



def load_cycle_table():
    """Per-opcode 68000 base cost in master clocks (x7), from the oracle core's table
    (pinned submodule; the numbers are the documented 68000 instruction timings). Without the
    submodule (the Mac app's tree): m68k_cycles.txt beside this file, the same numbers, written by
    engine/tools/macapp/make_dmg.sh."""
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, '..', '..', 'ext', 'Genesis-Plus-GX', 'core', 'm68k', 'm68ki_cycles.h')
    if not os.path.exists(path):
        vals = [int(v) for v in open(os.path.join(here, 'm68k_cycles.txt')).read().split()]
        assert len(vals) == 0x10000, len(vals)
        return vals
    txt = open(path).read()
    body = txt[txt.index('{') + 1: txt.rindex('}')]
    vals = [int(eval(v)) for v in re.findall(r'\d+\s*\*\s*\d+|\d+', body)]
    assert len(vals) == 0x10000, len(vals)
    return vals


CYCLES = load_cycle_table()


def timed(op, raw, c_code, addr=None):
    """Wrap one instruction's C with its cycle cost (see CYC/M68K_PRE in m68k.h)."""
    out = _timed(op, raw, c_code, addr)
    # a jump to a known address in another function: an interrupt there returns to it
    out = re.sub(r'CYC\((\d+)\); (\{ )?func_table_(tail|call)\((0x[0-9A-Fa-f]+)\)',
                 r'CYCP(\1, \4); \2func_table_\3(\4)', out)
    # a call (JSR/BSR) to a known address: an interrupt there returns to the callee (the JSR is
    # done), or to where the handler redirected it (CYCJ, m68k.h)
    out = re.sub(r'CYC\((\d+)\); (if \(!)?func_table_jsr\((0x[0-9A-Fa-f]+), (0x[0-9A-Fa-f]+)\)(\) return;|;)',
                 r'{ uint32_t _jt = \3; CYCJ(\1, _jt); \2func_table_jsr(_jt, \4)\5 }', out)
    # a return: at the address on the stack, or the handler's (CYCR) - with 0 a handler reading
    # its stacked PC saw 0 (SHK's VBlank tests it against the Kosinski decompressor's range)
    return re.sub(r'CYC\((\d+)\); \{ func_table_rts\(\);', r'CYCR(\1); { func_table_rts();', out)


def _timed(op, raw, c_code, addr=None):
    base = CYCLES[op]
    it = f'ITRACE(0x{addr:06X}); ' if addr is not None else ''
    if addr is not None and 'bus_read' in c_code:
        # 68000 prefetch word (open-bus/status upper bits) and this instruction's cost, for
        # reads whose result depends on them (VDP status is evaluated at instruction end)
        it += f'g_m68k_pf = 0x{addr + len(raw):06X}; g_m68k_icost = {base}; '
    hi = op & 0xF000
    # CYCP: the cost, and the PC an interrupt taken there returns to (see m68k.h)
    nxt = f'0x{(addr + len(raw)) & 0xFFFFFF:06X}' if addr is not None else None
    def cyc(n, pc=None):
        return f'CYCP({n}, {pc})' if pc else f'CYC({n})'
    if hi == 0x6000:                                    # BRA / BSR / Bcc
        cond = (op >> 8) & 0xF
        tgt = None
        if addr is not None:
            d = op & 0xFF
            d = (raw[2] << 8 | raw[3]) if d == 0 and len(raw) >= 4 else d
            d = d - 0x10000 if (op & 0xFF) == 0 and d >= 0x8000 else d - 0x100 if (op & 0xFF) and d >= 0x80 else d
            tgt = f'0x{(addr + 2 + d) & 0xFFFFFF:06X}'
        if cond >= 2:                                   # Bcc: not taken -2 (.b) / +2 (.w)
            adj = 14 if (op & 0xFF) == 0 else -14
            # charge each outcome's final cost once (no transient crossing of an event)
            c = re.sub(r'goto (\w+);', lambda m: f'{{ {cyc(base, tgt)}; goto {m.group(1)}; }}', c_code, count=1)
            if c == c_code:
                c = c_code.replace('{ func_table_call', f'{{ CYC({base}); func_table_call', 1)
            return f'{it}M68K_PRE(); {c} {cyc(base + adj, nxt)};'
        return f'{it}M68K_PRE(); {cyc(base, tgt if cond == 0 and "goto" in c_code else None)}; {c_code}'
    if (op & 0xF0F8) == 0x50C8:                         # DBcc: true = base, loop -2, expired +2
        tgt = None
        if addr is not None and len(raw) >= 4:
            d = raw[2] << 8 | raw[3]
            tgt = f'0x{(addr + 2 + (d - 0x10000 if d >= 0x8000 else d)) & 0xFFFFFF:06X}'
        c = re.sub(r'if \(_cnt != -1\) (goto \w+;|\{ func_table_call\([^;]*\); return; \})',
                   lambda m: f'if (_cnt != -1) {{ {cyc(base - 14, tgt if m.group(1).startswith("goto") else None)}; {m.group(1)} }} {cyc(base + 14, nxt)};', c_code)
        if c.startswith('if (!('):
            c += f' else {cyc(base, nxt)};'
        return f'{it}M68K_PRE(); {c}'
    if (op & 0xFFC0) in (0x4EC0, 0x4E80) or op in (0x4E75, 0x4E73, 0x4E77) or 'return' in c_code \
            or 'goto ' in c_code or 'func_table_' in c_code:
        return f'{it}M68K_PRE(); CYC({base}); {c_code}'
    pre, extra, post = '', 0, ''
    if hi == 0xE000 and ((op >> 6) & 3) != 3:         # shift/rotate register: +2 per bit
        if op & 0x20:
            r = (op >> 9) & 7
            pre = f'if (g_m68k.d[{r}] & 63) {{ g_cyc += (g_m68k.d[{r}] & 63) * 14; M68K_SKIPREF(); }} '
        else:
            extra = (((op >> 9) & 7) or 8) * 14
    elif (op & 0xFB80) == 0x4880 and len(raw) >= 4:   # MOVEM: +4 (.w) / +8 (.l) per register
        mask = (raw[2] << 8) | raw[3]
        n = bin(mask).count('1') * (8 if op & 0x40 else 4) * 7
        if op & 0x40:                                   # MOVEM.L: refresh check after the transfers
            return f'{it}M68K_PRE(); {c_code} g_cyc += {n}; M68K_SKIPREF(); {cyc(base, nxt)};'
        extra = n
    elif (op & 0xFFF8) in (0x0880, 0x08C0, 0x0840) and len(raw) >= 4:   # BCLR/BSET/BCHG #n,Dn
        if (raw[3] & 31) >= 16:
            extra = 14                                   # +2 for bits 16-31
    elif (op & 0xF1F8) in (0x0180, 0x01C0, 0x0140):   # BCLR/BSET/BCHG Dm,Dn
        pre = f'if ((g_m68k.d[{(op >> 9) & 7}] & 31) >= 16) g_cyc += 14; '
    elif (op & 0xF0C0) == 0x50C0 and ((op >> 3) & 7) == 0 and (op & 0x0F00):   # Scc Dn: +2 when true
        # (ST Dn, cc=0, is always true: its table cost is already 6, as in the oracle's m68k_op_st_8_d)
        post = f' if ((uint8_t)g_m68k.d[{op & 7}]) {cyc(14, nxt)};'
    return f'{it}M68K_PRE(); {pre}{c_code} {cyc(base + extra, nxt)};{post}'

class M68KTranslator:
    """Translates individual M68K instructions to C statements using genrecomp macros."""

    # Map capstone condition codes to M68K_CC_* macros
    CC_MAP = {
        'hi': 'M68K_CC_HI', 'ls': 'M68K_CC_LS',
        'cc': 'M68K_CC_CC', 'cs': 'M68K_CC_CS',
        'ne': 'M68K_CC_NE', 'eq': 'M68K_CC_EQ',
        'vc': 'M68K_CC_VC', 'vs': 'M68K_CC_VS',
        'pl': 'M68K_CC_PL', 'mi': 'M68K_CC_MI',
        'ge': 'M68K_CC_GE', 'lt': 'M68K_CC_LT',
        'gt': 'M68K_CC_GT', 'le': 'M68K_CC_LE',
    }

    SIZE_SUFFIX = {'.b': 8, '.w': 16, '.l': 32}

    def __init__(self, rom, labels):
        self.rom = rom
        self.labels = labels  # addr -> label name
        self.func_start = 0
        self.func_end = 0

    def translate_instruction(self, addr, mnemonic, op_str, raw_bytes, func_start, func_end=0):
        """Translate one M68K instruction to C code. Returns list of C lines."""
        self.func_start = func_start
        self.func_end = func_end
        lines = []

        # Add label if this address is a branch target
        # (unreferenced ones are dropped after the function is assembled)
        if addr in self.labels:
            lines.append(f"{self.labels[addr]}:")
        # Title hooks (recomp.json "hooks": address -> C run before the instruction),
        # e.g. widescreen enlarging an object activation range; "goto skip;" in a hook jumps
        # past the instruction (widescreen: keep a sprite's full x instead of its 9-bit mask)
        hook = getattr(self, 'hooks', {}).get(addr)
        skip = None
        if hook:
            if not hasattr(self, 'hook_len'): self.hook_len = {}
            self.hook_len[addr] = len(raw_bytes)  # the interpreter's copy skips by this (recomp_hooks.c)
            if 'goto skip;' in hook:              # the hook may skip this instruction
                skip = f"skip_{addr:06X}"
                hook = hook.replace('goto skip;', f'goto {skip};')
            lines.append(f"    {{ {hook} }} /* hook */")

        # Parse size suffix
        size = 16  # default word
        base_mnem = mnemonic
        for suffix, sz in self.SIZE_SUFFIX.items():
            if mnemonic.endswith(suffix):
                size = sz
                base_mnem = mnemonic[:-2]
                break

        cheat = getattr(self, 'cheat_sites', {}).get(addr)
        if cheat is not None:
            return lines + self._cheat_pair(cheat, addr, mnemonic, op_str, raw_bytes, func_start, func_end)

        # Dispatch to handler
        c_code = self._translate(addr, base_mnem, mnemonic, op_str, size, raw_bytes)
        if c_code and ('TODO' in c_code or 'UNHANDLED' in c_code or 'unresolved' in c_code):
            c_code = None                          # a placeholder, not code: the interpreter runs it
        if c_code:
            c_code = self._rmw_autoinc(c_code, op_str, size, mnemonic)
        if c_code:
            op = (raw_bytes[0] << 8) | raw_bytes[1]
            lines.append(f"    {timed(op, raw_bytes, c_code, addr)}")
        else:
            # Not translated: the interpreter runs from this instruction on, as for code never
            # found (func_table.c's miss path); it charges the instruction's cycles itself.
            # Never skipped: before, the instruction was left out as a comment.
            hex_str = ' '.join(f'{b:02X}' for b in raw_bytes)
            lines.append(f"    {{ m68k_interp_run(0x{addr:06X}); return; }} "
                         f"/* not translated: {mnemonic} {op_str}  [{hex_str}] */")
        if skip:
            lines.append(f"{skip}:;")

        return lines

    def _cheat_pair(self, n, addr, mnemonic, op_str, raw_bytes, func_start, func_end):
        """Patched and original instruction behind a runtime cheat switch."""
        sites, self.cheat_sites = self.cheat_sites, {}
        labels = self.labels; self.labels = {}
        try:
            md = Cs(CS_ARCH_M68K, CS_MODE_M68K_000)
            o = next(md.disasm(self.orig_data[addr:addr + 10], addr))
            on = self.translate_instruction(addr, mnemonic, op_str, raw_bytes, func_start, func_end)
            off = self.translate_instruction(addr, o.mnemonic, o.op_str, bytes(o.bytes), func_start, func_end)
        finally:
            self.cheat_sites, self.labels = sites, labels
        ends = ('rts', 'rte', 'rtr', 'jmp', 'bra')
        if mnemonic.split('.')[0] in ends and o.mnemonic.split('.')[0] not in ends:
            # the patched instruction ends the flow, so what is emitted next is not the
            # original's fall-through: continue there explicitly (an entry point, see main)
            off.append(f"    func_table_tail(0x{addr + o.size:06X}); return;")
        out = [f"    if (ps_cheat({n})) {{ /* cheat {self.cheat_names[n]} */"] + on + ["    } else {"] + off + ["    }"]
        return out

    def _rmw_autoinc(self, c_code, op_str, size, mnemonic):
        """Read-modify-write on (An)+ / -(An): the address helper leaves the register update
        to the caller; apply it here (byte steps on A7 are 2)."""
        ops = self._split_operands(op_str) if op_str else []
        for o in ops:
            o = o.strip().lower()
            m = re.match(r'^\(a([0-7])\)\+$', o) or re.match(r'^-\(a([0-7])\)$', o)
            if not m:
                continue
            n = int(m.group(1))
            cap = f'uint32_t _ea = g_m68k.a[{n}];'
            if cap not in c_code or f'g_m68k.a[{n}] +=' in c_code or f'g_m68k.a[{n}] -=' in c_code:
                continue
            sz = 16 if mnemonic.split('.')[0] in ('asl', 'asr', 'lsl', 'lsr', 'rol', 'ror', 'roxl', 'roxr') else size
            step = {8: 2 if n == 7 else 1, 16: 2, 32: 4}[sz]
            if o.startswith('('):
                c_code = c_code.replace(cap, f'{cap} g_m68k.a[{n}] += {step};', 1)
            else:
                c_code = c_code.replace(cap, f'g_m68k.a[{n}] -= {step}; {cap}', 1)
        return c_code

    def _translate(self, addr, base, mnemonic, op_str, size, raw_bytes):
        """Core translation dispatch."""
        self._ret = addr + len(raw_bytes)   # return address for BSR/JSR
        ops = self._split_operands(op_str) if op_str else []

        # ---- MOVE family ----
        # Check SR/CCR first before generic move
        if base == 'move' and any(o.strip().lower() in ('sr', 'ccr') for o in ops):
            return self._gen_move_sr(ops, mnemonic)
        if base == 'move' and any(o.strip().lower() == 'usp' for o in ops):
            # MOVE USP,An / MOVE An,USP: always a long, flags untouched
            return f'{self._reg(ops[1])} = {self._reg(ops[0])};'
        if base == 'move':
            return self._gen_move(ops, size)
        if base == 'movea':
            return self._gen_movea(ops, size)
        if base == 'moveq':
            return self._gen_moveq(ops)
        if mnemonic == 'movem.l' or mnemonic == 'movem.w':
            return self._gen_movem(ops, mnemonic, size)
        if base == 'clr':
            return self._gen_clr(ops, size)
        if base == 'lea':
            return self._gen_lea(ops)
        if base == 'pea':
            return self._gen_pea(ops)
        if base == 'exg' or mnemonic == 'exg':
            return self._gen_exg(ops)
        if base == 'ext':
            return self._gen_ext(ops, size)
        if mnemonic == 'swap':
            return self._gen_swap(ops)
        if base == 'link':
            return self._gen_link(ops)
        if mnemonic == 'unlk':
            return self._gen_unlk(ops)

        # ---- Arithmetic ----
        if base == 'add':
            return self._gen_arith('ADD', ops, size)
        if base == 'adda':
            return self._gen_adda(ops, size)
        if base == 'addi':
            return self._gen_arith('ADD', ops, size)
        if base == 'addq':
            return self._gen_addq(ops, size)
        if base in ('addx', 'subx') and len(ops) == 2 and self._predec(ops[0]) is not None \
                and self._predec(ops[1]) is not None:
            y, x, sz = self._predec(ops[0]), self._predec(ops[1]), size
            return (f'{{ g_m68k.a[{y}] -= {self._step(y, sz)}; uint{sz}_t _s = bus_read{sz}(g_m68k.a[{y}]); '
                    f'g_m68k.a[{x}] -= {self._step(x, sz)}; uint{sz}_t _t = bus_read{sz}(g_m68k.a[{x}]); '
                    f'M68K_{base.upper()}{sz}(_t, _s); bus_write{sz}(g_m68k.a[{x}], _t); }}')
        if base == 'addx':
            return self._gen_arith('ADDX', ops, size)
        if base == 'sub':
            return self._gen_arith('SUB', ops, size)
        if base == 'suba':
            return self._gen_suba(ops, size)
        if base == 'subi':
            return self._gen_arith('SUB', ops, size)
        if base == 'subq':
            return self._gen_subq(ops, size)
        if base == 'subx':
            return self._gen_arith('SUBX', ops, size)
        if base == 'cmp':
            return self._gen_cmp(ops, size)
        if base == 'cmpa':
            return self._gen_cmpa(ops, size)
        if base == 'cmpi':
            return self._gen_cmp(ops, size)
        if base == 'neg':
            return self._gen_neg('NEG', ops, size)
        if base == 'negx':
            return self._gen_neg('NEGX', ops, size)
        if base == 'mulu':
            return self._gen_mul('MULU', ops)
        if base == 'muls':
            return self._gen_mul('MULS', ops)
        if base == 'divu':
            return self._gen_div('DIVU', ops)
        if base == 'divs':
            return self._gen_div('DIVS', ops)

        # ---- Logic ----
        # Check for ANDI/ORI/EORI to SR/CCR first
        if base in ('andi', 'ori', 'eori') and len(ops) == 2:
            dst_lower = ops[1].strip().lower()
            if dst_lower == 'sr':
                imm = self._imm(ops[0])
                if base == 'andi':
                    return f'm68k_set_sr(m68k_get_sr() & {imm});'
                elif base == 'ori':
                    return f'm68k_set_sr(m68k_get_sr() | {imm});'
                else:
                    return f'm68k_set_sr(m68k_get_sr() ^ {imm});'
            if dst_lower == 'ccr':
                imm = self._imm(ops[0])
                if base == 'andi':
                    return f'm68k_set_ccr(m68k_get_ccr() & {imm});'
                elif base == 'ori':
                    return f'm68k_set_ccr(m68k_get_ccr() | {imm});'
                else:
                    return f'm68k_set_ccr(m68k_get_ccr() ^ {imm});'
        if base in ('and', 'andi'):
            return self._gen_logic('AND', ops, size)
        if base in ('or', 'ori'):
            return self._gen_logic('OR', ops, size)
        if base in ('eor', 'eori'):
            return self._gen_logic('EOR', ops, size)
        if base == 'not':
            return self._gen_unary_logic('NOT', ops, size)
        if base == 'tst':
            return self._gen_tst(ops, size)
        if base in ('abcd', 'sbcd'):
            return self._gen_bcd(base.upper(), ops)
        if base == 'nbcd':
            return self._gen_nbcd(ops)
        if base == 'cmpm':
            return self._gen_cmpm(ops, size)
        if base == 'movep':
            return self._gen_movep(ops, size)
        if base == 'chk':
            return self._gen_chk(ops)

        # ---- Bit operations ----
        if base == 'btst':
            return self._gen_btst(ops)
        if base == 'bset':
            return self._gen_bset(ops)
        if base == 'bclr':
            return self._gen_bclr(ops)
        if base == 'bchg':
            return self._gen_bchg(ops)

        # ---- Shifts ----
        if base == 'lsl':
            return self._gen_shift('LSL', ops, size)
        if base == 'lsr':
            return self._gen_shift('LSR', ops, size)
        if base == 'asl':
            return self._gen_shift('ASL', ops, size)
        if base == 'asr':
            return self._gen_shift('ASR', ops, size)
        if base == 'rol':
            return self._gen_shift('ROL', ops, size)
        if base == 'ror':
            return self._gen_shift('ROR', ops, size)
        if base == 'roxl':
            return self._gen_shift('ROXL', ops, size)
        if base == 'roxr':
            return self._gen_shift('ROXR', ops, size)

        # ---- Branch ----
        if base == 'bra':
            return self._gen_bra(ops, addr)
        if base == 'bsr':
            return self._gen_bsr(ops, addr)
        if base == 'jmp':
            return self._gen_jmp(ops, addr)
        if base == 'jsr':
            return self._gen_jsr(ops, addr)
        if mnemonic == 'rts':
            return '{ func_table_rts(); return; }'
        if mnemonic == 'rte':
            return '{ uint32_t _t; if (m68k_rte(&_t)) func_table_tail(_t); return; } /* RTE */'
        if mnemonic == 'rtr':
            return '{ uint16_t _sr = bus_read16(g_m68k.a[7]); g_m68k.a[7] += 2; m68k_set_ccr((uint8_t)_sr); func_table_rts(); return; }'

        # Conditional branches
        for cc_suffix, cc_macro in self.CC_MAP.items():
            if base == f'b{cc_suffix}':
                return self._gen_bcc(cc_macro, ops, addr)
            if base == f's{cc_suffix}':
                return self._gen_scc(cc_macro, ops)
            if base == f'db{cc_suffix}':
                return self._gen_dbcc(cc_macro, ops, addr)

        if mnemonic == 'dbra':
            return self._gen_dbcc('0', ops, addr)  # DBRA = DBF
        if base == 'dbt':                          # condition true: never decrements, never loops
            return self._gen_dbcc('M68K_CC_T', ops, addr)
        if base == 'tas':
            return self._gen_tas(ops)
        if mnemonic == 'trapv':
            return 'if (g_m68k.flag_V) recomp_m68k_exception(7); else g_cyc += 4 * 7;'   # V clear: 4 clocks
        if mnemonic == 'reset':
            # pulses the external reset line; nothing in the CPU changes (the console's hardware
            # does not reset from it); a privileged instruction like MOVE to SR
            return 'g_cyc += 132 * 7; g_refresh += 128 * 7; /* reset: 132 clocks, one refresh skipped */'
        if base == 'st' or mnemonic == 'st':
            return self._gen_st(ops)
        if base == 'sf' or mnemonic == 'sf':
            return self._gen_sf(ops)

        # ---- Stack ----
        if mnemonic == 'nop':
            return '/* nop */'

        # ---- SR/CCR ----
        if 'andi' in mnemonic and 'sr' in op_str.lower():
            return self._gen_andi_sr(ops)
        if 'ori' in mnemonic and 'sr' in op_str.lower():
            return self._gen_ori_sr(ops)

        # ---- TRAP ----
        if base == 'trap':
            # TRAP #N uses vector 32+N (address (32+N)*4 in vector table)
            trap_num = self._imm(ops[0])
            return f'recomp_m68k_exception(32 + {trap_num}); /* TRAP #{trap_num} */'

        return None  # unhandled

    # ================================================================
    # Operand helpers
    # ================================================================

    @staticmethod
    def _split_operands(op_str):
        """Split operand string by comma, respecting parentheses nesting."""
        parts = []
        depth = 0
        current = []
        for ch in op_str:
            if ch == '(' or ch == '[':
                depth += 1
                current.append(ch)
            elif ch == ')' or ch == ']':
                depth -= 1
                current.append(ch)
            elif ch == ',' and depth == 0:
                parts.append(''.join(current).strip())
                current = []
            else:
                current.append(ch)
        if current:
            parts.append(''.join(current).strip())
        return parts

    @staticmethod
    def _parse_reglist(reglist_str):
        """Parse M68K register list string into list of register names.
        E.g. 'd0-d3/a2-a4' -> ['d0','d1','d2','d3','a2','a3','a4']
        """
        regs = []
        # Split by '/'
        for part in reglist_str.replace(' ', '').lower().split('/'):
            m = re.match(r'^([da])(\d)-([da])(\d)$', part)
            if m:
                kind = m.group(1)
                start = int(m.group(2))
                end = int(m.group(4))
                for i in range(start, end + 1):
                    regs.append(f'{kind}{i}')
            else:
                m2 = re.match(r'^([da])(\d)$', part)
                if m2:
                    regs.append(f'{m2.group(1)}{m2.group(2)}')
                elif part == 'sp':
                    regs.append('a7')
        return regs

    def _reg(self, op):
        """Convert register name to C expression."""
        op = op.strip().lower()
        if re.match(r'^d[0-7]$', op):
            return f'g_m68k.d[{op[1]}]'
        if re.match(r'^a[0-7]$', op):
            return f'g_m68k.a[{op[1]}]'
        if op == 'sp':
            return 'g_m68k.a[7]'
        if op == 'sr':
            return 'm68k_get_sr()'
        if op == 'ccr':
            return 'm68k_get_ccr()'
        if op == 'usp':
            return 'g_m68k.usp'
        return None

    def _imm(self, op):
        """Extract immediate value."""
        op = op.strip()
        if op.startswith('#$'):
            return f'0x{op[2:]}'
        if op.startswith('#0x'):
            return op[1:]
        if op.startswith('#-$'):
            return f'(-0x{op[3:]})'
        if op.startswith('#-'):
            return op[1:]
        if op.startswith('#'):
            return op[1:]
        return None

    def _ea_read(self, op, size):
        """Generate C expression to READ from an effective address."""
        op = op.strip()

        # Register direct
        r = self._reg(op)
        if r:
            return r

        # Immediate
        imm = self._imm(op)
        if imm:
            return imm

        # (An) - register indirect
        m = re.match(r'^\(a([0-7])\)$', op, re.I)
        if m:
            return f'bus_read{size}(g_m68k.a[{m.group(1)}])'

        # (An)+ - postincrement (use helper to avoid GCC statement expressions)
        m = re.match(r'^\(a([0-7])\)\+$', op, re.I)
        if m:
            n = m.group(1)
            return f'_postinc{size}({n})'

        # -(An) - predecrement
        m = re.match(r'^-\(a([0-7])\)$', op, re.I)
        if m:
            n = m.group(1)
            return f'_predec{size}({n})'

        # d(An) - displacement
        m = re.match(r'^(-?\$?[\dA-Fa-f]+)\(a([0-7])\)$', op, re.I)
        if m:
            disp = self._parse_disp(m.group(1))
            return f'bus_read{size}(g_m68k.a[{m.group(2)}] + {disp})'

        # d(An,Xn.s) or d(An,Xn.s * scale) - indexed with data or address register
        m = re.match(r'^(-?\$?[\dA-Fa-f]*)\(a([0-7]),\s*([da])([0-7])(?:\.(w|l))?(?:\s*\*\s*(\d+))?\)$', op, re.I)
        if m:
            disp = self._parse_disp(m.group(1)) if m.group(1) else '0'
            idx_kind = m.group(3).lower()
            idx_num = m.group(4)
            idx_size = (m.group(5) or 'w').lower()
            scale = m.group(6)
            if idx_kind == 'd':
                idx = f'g_m68k.d[{idx_num}]'
            else:
                idx = f'g_m68k.a[{idx_num}]'
            if idx_size == 'w':
                idx = f'(int16_t)(uint16_t){idx}'
            else:
                idx = f'(int32_t){idx}'
            if scale and scale != '1':
                idx = f'{idx} * {scale}'
            return f'bus_read{size}(g_m68k.a[{m.group(2)}] + {disp} + {idx})'

        # d(PC) - PC relative
        m = re.match(r'^(-?\$?[\dA-Fa-f]+)\(pc\)$', op, re.I)
        if m:
            disp = self._parse_disp(m.group(1))
            return f'bus_read{size}({disp})'  # PC-relative resolved by capstone

        # d(PC, Xn.s) - PC relative indexed
        m = re.match(r'^(-?\$?[\dA-Fa-f]*)\(pc,\s*([da])([0-7])(?:\.(w|l))?(?:\s*\*\s*(\d+))?\)$', op, re.I)
        if m:
            disp = self._parse_disp(m.group(1)) if m.group(1) else '0'
            idx_kind = m.group(2).lower()
            idx_num = m.group(3)
            idx_size = (m.group(4) or 'w').lower()
            scale = m.group(5)
            if idx_kind == 'd':
                idx = f'g_m68k.d[{idx_num}]'
            else:
                idx = f'g_m68k.a[{idx_num}]'
            if idx_size == 'w':
                idx = f'(int16_t)(uint16_t){idx}'
            else:
                idx = f'(int32_t){idx}'
            if scale and scale != '1':
                idx = f'{idx} * {scale}'
            return f'bus_read{size}({disp} + {idx})'

        # ($XXXX).w or ($XXXX).l - absolute
        m = re.match(r'^\(\$([0-9A-Fa-f]+)\)\.(w|l)$', op, re.I)
        if m:
            addr_val = _abs_addr(m)
            return f'bus_read{size}(0x{addr_val:06X})'

        # $XXXX.l or $XXXX - absolute (no parens)
        m = re.match(r'^\$([0-9A-Fa-f]+)(?:\.(w|l))?$', op, re.I)
        if m:
            addr_val = _abs_addr(m)
            return f'bus_read{size}(0x{addr_val:06X})'

        return f'/* UNHANDLED_READ: {op} */ 0'

    def _ea_write(self, op, size, value_expr):
        """Generate C statement to WRITE to an effective address."""
        op = op.strip()

        # Register direct
        r = self._reg(op)
        if r:
            if size == 8:
                return f'{r} = ({r} & 0xFFFFFF00u) | ((uint8_t)({value_expr}));'
            elif size == 16:
                return f'{r} = ({r} & 0xFFFF0000u) | ((uint16_t)({value_expr}));'
            else:
                return f'{r} = {value_expr};'

        # (An)
        m = re.match(r'^\(a([0-7])\)$', op, re.I)
        if m:
            return f'bus_write{size}(g_m68k.a[{m.group(1)}], {value_expr});'

        # (An)+
        m = re.match(r'^\(a([0-7])\)\+$', op, re.I)
        if m:
            n = m.group(1)
            inc = 2 if (size == 8 and n == '7') else size // 8   # A7 stays word-aligned
            return f'bus_write{size}(g_m68k.a[{n}], {value_expr}); g_m68k.a[{n}] += {inc};'

        # -(An)
        m = re.match(r'^-\(a([0-7])\)$', op, re.I)
        if m:
            n = m.group(1)
            dec = 2 if (size == 8 and n == '7') else size // 8   # A7 stays word-aligned
            return f'g_m68k.a[{n}] -= {dec}; bus_write{size}(g_m68k.a[{n}], {value_expr});'

        # d(An)
        m = re.match(r'^(-?\$?[\dA-Fa-f]+)\(a([0-7])\)$', op, re.I)
        if m:
            disp = self._parse_disp(m.group(1))
            return f'bus_write{size}(g_m68k.a[{m.group(2)}] + {disp}, {value_expr});'

        # d(An,Xn.s) or d(An,Xn.s * scale) - indexed with data or address register
        m = re.match(r'^(-?\$?[\dA-Fa-f]*)\(a([0-7]),\s*([da])([0-7])(?:\.(w|l))?(?:\s*\*\s*(\d+))?\)$', op, re.I)
        if m:
            disp = self._parse_disp(m.group(1)) if m.group(1) else '0'
            idx_kind = m.group(3).lower()
            idx_num = m.group(4)
            idx_size = (m.group(5) or 'w').lower()
            scale = m.group(6)
            if idx_kind == 'd':
                idx = f'g_m68k.d[{idx_num}]'
            else:
                idx = f'g_m68k.a[{idx_num}]'
            if idx_size == 'w':
                idx = f'(int16_t)(uint16_t){idx}'
            else:
                idx = f'(int32_t){idx}'
            if scale and scale != '1':
                idx = f'{idx} * {scale}'
            return f'bus_write{size}(g_m68k.a[{m.group(2)}] + {disp} + {idx}, {value_expr});'

        # Absolute
        m = re.match(r'^\(\$([0-9A-Fa-f]+)\)\.(w|l)$', op, re.I)
        if m:
            addr_val = _abs_addr(m)
            return f'bus_write{size}(0x{addr_val:06X}, {value_expr});'

        m = re.match(r'^\$([0-9A-Fa-f]+)(?:\.(w|l))?$', op, re.I)
        if m:
            addr_val = _abs_addr(m)
            return f'bus_write{size}(0x{addr_val:06X}, {value_expr});'

        return f'/* UNHANDLED_WRITE: {op} = {value_expr} */'

    def _parse_disp(self, s):
        """Parse displacement value."""
        s = s.strip()
        if not s or s == '0':
            return '0'
        neg = s.startswith('-')
        if neg:
            s = s[1:]
        if s.startswith('$'):
            val = f'0x{s[1:]}'
        elif s.startswith('0x'):
            val = s
        else:
            val = s
        return f'(-{val})' if neg else val

    def _ea_addr(self, op):
        """Generate C expression for the effective address itself (for LEA/PEA/memory RMW)."""
        op = op.strip()

        # (An)+  — for _ea_addr in RMW context, just use current An
        m = re.match(r'^\(a([0-7])\)\+$', op, re.I)
        if m:
            return f'g_m68k.a[{m.group(1)}]'

        # -(An) — for _ea_addr in RMW context, pre-decrement is caller's job
        m = re.match(r'^-\(a([0-7])\)$', op, re.I)
        if m:
            return f'g_m68k.a[{m.group(1)}]'

        m = re.match(r'^\(a([0-7])\)$', op, re.I)
        if m:
            return f'g_m68k.a[{m.group(1)}]'

        m = re.match(r'^(-?\$?[\dA-Fa-f]+)\(a([0-7])\)$', op, re.I)
        if m:
            disp = self._parse_disp(m.group(1))
            return f'(g_m68k.a[{m.group(2)}] + {disp})'

        m = re.match(r'^(-?\$?[\dA-Fa-f]*)\(a([0-7]),\s*([da])([0-7])(?:\.(w|l))?(?:\s*\*\s*(\d+))?\)$', op, re.I)
        if m:
            disp = self._parse_disp(m.group(1)) if m.group(1) else '0'
            idx_kind = m.group(3).lower()
            idx_num = m.group(4)
            idx_size = (m.group(5) or 'w').lower()
            scale = m.group(6)
            if idx_kind == 'd':
                idx = f'g_m68k.d[{idx_num}]'
            else:
                idx = f'g_m68k.a[{idx_num}]'
            if idx_size == 'w':
                idx = f'(int16_t)(uint16_t){idx}'
            else:
                idx = f'(int32_t){idx}'
            if scale and scale != '1':
                idx = f'{idx} * {scale}'
            return f'(g_m68k.a[{m.group(2)}] + {disp} + {idx})'

        m = re.match(r'^(-?\$?[\dA-Fa-f]+)\(pc\)$', op, re.I)
        if m:
            return self._parse_disp(m.group(1))

        # d(PC, Xn.s) - PC relative indexed
        m = re.match(r'^(-?\$?[\dA-Fa-f]*)\(pc,\s*([da])([0-7])(?:\.(w|l))?(?:\s*\*\s*(\d+))?\)$', op, re.I)
        if m:
            disp = self._parse_disp(m.group(1)) if m.group(1) else '0'
            idx_kind = m.group(2).lower()
            idx_num = m.group(3)
            idx_size = (m.group(4) or 'w').lower()
            scale = m.group(5)
            if idx_kind == 'd':
                idx = f'g_m68k.d[{idx_num}]'
            else:
                idx = f'g_m68k.a[{idx_num}]'
            if idx_size == 'w':
                idx = f'(int16_t)(uint16_t){idx}'
            else:
                idx = f'(int32_t){idx}'
            if scale and scale != '1':
                idx = f'{idx} * {scale}'
            return f'({disp} + {idx})'

        m = re.match(r'^\(\$([0-9A-Fa-f]+)\)\.(w|l)$', op, re.I)
        if m:
            return f'0x{_abs_addr(m):06X}'

        m = re.match(r'^\$([0-9A-Fa-f]+)(?:\.(w|l))?$', op, re.I)
        if m:
            return f'0x{_abs_addr(m):06X}'

        return f'/* UNHANDLED_ADDR: {op} */ 0'

    # ================================================================
    # Instruction generators
    # ================================================================

    def _gen_move(self, ops, size):
        if len(ops) != 2:
            return None
        src = self._ea_read(ops[0], size)
        # MOVE updates N, Z, clears V, C
        r = self._reg(ops[1])
        if r:
            stmt = self._ea_write(ops[1], size, src)
            mask = {8: '0xFF', 16: '0xFFFF', 32: '0xFFFFFFFFu'}[size]
            return f'{stmt} M68K_TST{size}({r} & {mask});'
        # Memory destination: use temp for flag testing
        return f'{{ uint{size}_t _mv = (uint{size}_t)({src}); {self._ea_write(ops[1], size, "_mv")} M68K_TST{size}(_mv); }}'

    def _gen_movea(self, ops, size):
        if len(ops) != 2:
            return None
        src = self._ea_read(ops[0], size)
        r = self._reg(ops[1])
        if r:
            if size == 16:
                return f'{r} = (uint32_t)(int32_t)(int16_t)(uint16_t)({src});'
            return f'{r} = {src};'
        return None

    def _gen_moveq(self, ops):
        if len(ops) != 2:
            return None
        imm = self._imm(ops[0])
        r = self._reg(ops[1])
        if imm and r:
            return f'{r} = (uint32_t)(int32_t)(int8_t)({imm}); M68K_TST32({r});'
        return None

    def _gen_movem(self, ops, mnemonic, size):
        byte_size = size // 8  # 2 for .w, 4 for .l
        read_fn = f'bus_read{size}'
        write_fn = f'bus_write{size}'

        # Determine direction: register-to-memory or memory-to-register
        # movem <reglist>, <ea>  -- store regs to memory
        # movem <ea>, <reglist>  -- load regs from memory

        # Check if first operand is a register list (contains d or a with ranges/slashes)
        def _is_reglist(s):
            s = s.strip().lower()
            return bool(re.match(r'^[da]\d', s)) and ('/' in s or '-' in s or re.match(r'^[da]\d$', s))

        if _is_reglist(ops[0]):
            # Store: movem <reglist>, <ea>
            reglist = self._parse_reglist(ops[0])
            ea = ops[1].strip()

            # -(An) predecrement: push in reverse order
            m = re.match(r'^-\(a([0-7])\)$', ea, re.I)
            if m:
                n = m.group(1)
                stmts = [f'{{ uint32_t _an = g_m68k.a[{n}];']
                # Reverse order: A7..A0 then D7..D0, but only regs in list
                all_regs_reverse = [f'a{i}' for i in range(7, -1, -1)] + [f'd{i}' for i in range(7, -1, -1)]
                for r in all_regs_reverse:
                    if r in reglist:
                        # 68000: An in its own list is stored as its initial value
                        reg_c = '_an' if r == f'a{n}' else self._reg(r)
                        stmts.append(f'g_m68k.a[{n}] -= {byte_size}; {write_fn}(g_m68k.a[{n}], {reg_c});')
                stmts.append('}')
                return ' '.join(stmts)

            # (An) no update: store with temp addr
            m = re.match(r'^\(a([0-7])\)$', ea, re.I)
            if m:
                n = m.group(1)
                stmts = [f'{{ uint32_t _addr = g_m68k.a[{n}];']
                for r in reglist:
                    reg_c = self._reg(r)
                    stmts.append(f'{write_fn}(_addr, {reg_c}); _addr += {byte_size};')
                stmts.append('}')
                return ' '.join(stmts)

            # d(An): store with displacement
            m = re.match(r'^(-?\$?[\dA-Fa-f]+)\(a([0-7])\)$', ea, re.I)
            if m:
                disp = self._parse_disp(m.group(1))
                n = m.group(2)
                stmts = [f'{{ uint32_t _addr = g_m68k.a[{n}] + {disp};']
                for r in reglist:
                    reg_c = self._reg(r)
                    stmts.append(f'{write_fn}(_addr, {reg_c}); _addr += {byte_size};')
                stmts.append('}')
                return ' '.join(stmts)

            # any other control mode (absolute .w/.l, d(An,Xn)): registers in list order
            addr = self._ea_addr(ea)
            if 'UNHANDLED' not in addr:
                stmts = [f'{{ uint32_t _addr = {addr};']
                for r in reglist:
                    stmts.append(f'{write_fn}(_addr, {self._reg(r)}); _addr += {byte_size};')
                stmts.append('}')
                return ' '.join(stmts)

            return f'/* TODO: movem store to {ea} */'

        else:
            # Load: movem <ea>, <reglist>
            reglist = self._parse_reglist(ops[1])
            ea = ops[0].strip()

            # (An)+ postincrement
            m = re.match(r'^\(a([0-7])\)\+$', ea, re.I)
            if m:
                n = m.group(1)
                stmts = []
                for r in reglist:
                    reg_c = self._reg(r)
                    if r == f'a{n}':
                        stmts.append(f'g_m68k.a[{n}] += {byte_size}; /* {r} itself: memory value discarded */')
                    elif size == 16:
                        stmts.append(f'{reg_c} = (uint32_t)(int32_t)(int16_t){read_fn}(g_m68k.a[{n}]); g_m68k.a[{n}] += {byte_size};')
                    else:
                        stmts.append(f'{reg_c} = {read_fn}(g_m68k.a[{n}]); g_m68k.a[{n}] += {byte_size};')
                return ' '.join(stmts)

            # (An) no update
            m = re.match(r'^\(a([0-7])\)$', ea, re.I)
            if m:
                n = m.group(1)
                stmts = [f'{{ uint32_t _addr = g_m68k.a[{n}];']
                for r in reglist:
                    reg_c = self._reg(r)
                    if size == 16:
                        stmts.append(f'{reg_c} = (uint32_t)(int32_t)(int16_t){read_fn}(_addr); _addr += {byte_size};')
                    else:
                        stmts.append(f'{reg_c} = {read_fn}(_addr); _addr += {byte_size};')
                stmts.append('}')
                return ' '.join(stmts)

            # d(An) displacement
            m = re.match(r'^(-?\$?[\dA-Fa-f]+)\(a([0-7])\)$', ea, re.I)
            if m:
                disp = self._parse_disp(m.group(1))
                n = m.group(2)
                stmts = [f'{{ uint32_t _addr = g_m68k.a[{n}] + {disp};']
                for r in reglist:
                    reg_c = self._reg(r)
                    if size == 16:
                        stmts.append(f'{reg_c} = (uint32_t)(int32_t)(int16_t){read_fn}(_addr); _addr += {byte_size};')
                    else:
                        stmts.append(f'{reg_c} = {read_fn}(_addr); _addr += {byte_size};')
                stmts.append('}')
                return ' '.join(stmts)

            # any other control mode (absolute .w/.l, d(An,Xn), d(PC), d(PC,Xn)); the address is
            # taken before any register of the list is loaded (an index register may be in it)
            addr = self._ea_addr(ea)
            if 'UNHANDLED' not in addr:
                stmts = [f'{{ uint32_t _addr = {addr};']
                for r in reglist:
                    reg_c = self._reg(r)
                    if size == 16:
                        stmts.append(f'{reg_c} = (uint32_t)(int32_t)(int16_t){read_fn}(_addr); _addr += {byte_size};')
                    else:
                        stmts.append(f'{reg_c} = {read_fn}(_addr); _addr += {byte_size};')
                stmts.append('}')
                return ' '.join(stmts)

            return f'/* TODO: movem load from {ea} */'

    def _gen_clr(self, ops, size):
        if len(ops) != 1:
            return None
        stmt = self._ea_write(ops[0], size, '0')
        return f'{stmt} g_m68k.flag_N = false; g_m68k.flag_Z = true; g_m68k.flag_V = false; g_m68k.flag_C = false;'

    def _gen_lea(self, ops):
        if len(ops) != 2:
            return None
        addr_expr = self._ea_addr(ops[0])
        r = self._reg(ops[1])
        if r:
            return f'{r} = {addr_expr};'
        return None

    def _gen_pea(self, ops):
        addr_expr = self._ea_addr(ops[0])
        return f'{{ uint32_t _pea = {addr_expr}; g_m68k.a[7] -= 4; bus_write32(g_m68k.a[7], _pea); }}'

    def _gen_exg(self, ops):
        r1 = self._reg(ops[0])
        r2 = self._reg(ops[1])
        if r1 and r2:
            return f'{{ uint32_t _t = {r1}; {r1} = {r2}; {r2} = _t; }}'
        return None

    def _gen_ext(self, ops, size):
        r = self._reg(ops[0])
        if r:
            if size == 16:
                return f'M68K_EXT16({r});'
            return f'M68K_EXT32({r});'
        return None

    def _gen_swap(self, ops):
        r = self._reg(ops[0])
        if r:
            return f'M68K_SWAP({r});'
        return None

    # -(An) / (An)+ operand -> register number, else None
    @staticmethod
    def _step(n, size):
        """(An)+ / -(An) step: the operand size, 2 for a byte through A7 (the stack stays even)."""
        return 2 if size == 8 and n == 7 else size // 8

    def _byte_store(self, op, value):
        """Byte write to a memory EA, with the (An)+ / -(An) register update."""
        n = self._postinc(op)
        if n is not None:
            return f'{{ uint32_t _ea = g_m68k.a[{n}]; g_m68k.a[{n}] += {self._step(n, 8)}; bus_write8(_ea, {value}); }}'
        n = self._predec(op)
        if n is not None:
            return f'g_m68k.a[{n}] -= {self._step(n, 8)}; bus_write8(g_m68k.a[{n}], {value});'
        return f'bus_write8({self._ea_addr(op)}, {value});'

    @staticmethod
    def _predec(op):
        m = re.fullmatch(r'-\(a([0-7])\)', op.strip().lower())
        return int(m.group(1)) if m else None

    @staticmethod
    def _postinc(op):
        m = re.fullmatch(r'\(a([0-7])\)\+', op.strip().lower())
        return int(m.group(1)) if m else None

    def _gen_bcd(self, op, ops):
        """ABCD/SBCD Dy,Dx or -(Ay),-(Ax)."""
        if len(ops) != 2:
            return None
        src, dst = self._reg(ops[0]), self._reg(ops[1])
        if src and dst:
            return f'M68K_{op}({dst}, {src});'
        y, x = self._predec(ops[0]), self._predec(ops[1])
        if y is None or x is None:
            return None
        return (f'{{ g_m68k.a[{y}] -= {self._step(y, 8)}; uint8_t _s = bus_read8(g_m68k.a[{y}]); '
                f'g_m68k.a[{x}] -= {self._step(x, 8)}; uint8_t _t = bus_read8(g_m68k.a[{x}]); '
                f'M68K_{op}(_t, _s); bus_write8(g_m68k.a[{x}], _t); }}')

    def _gen_nbcd(self, ops):
        r = self._reg(ops[0])
        if r:
            return f'M68K_NBCD({r});'
        ea_addr = self._ea_addr(ops[0])
        return f'{{ uint32_t _ea = {ea_addr}; uint8_t _t = bus_read8(_ea); M68K_NBCD(_t); bus_write8(_ea, _t); }}'

    def _gen_cmpm(self, ops, size):
        """CMPM (Ay)+,(Ax)+: compare (Ax) - (Ay), both post-incremented."""
        y, x = self._postinc(ops[0]), self._postinc(ops[1])
        if y is None or x is None:
            return None
        return (f'{{ uint{size}_t _s = bus_read{size}(g_m68k.a[{y}]); g_m68k.a[{y}] += {self._step(y, size)}; '
                f'uint{size}_t _d = bus_read{size}(g_m68k.a[{x}]); g_m68k.a[{x}] += {self._step(x, size)}; '
                f'M68K_CMP{size}(_d, _s); }}')

    def _gen_movep(self, ops, size):
        """MOVEP: register <-> alternate bytes at d16(An), high byte first.
        Used for 8-bit peripherals on one half of the bus (the Z80 side)."""
        n = size // 8
        src_r, dst_r = self._reg(ops[0]), self._reg(ops[1])
        if src_r and not dst_r:          # Dn -> d16(An)
            ea = self._ea_addr(ops[1])
            return '{ uint32_t _ea = ' + ea + '; ' + ' '.join(
                f'bus_write8(_ea + {2 * i}, (uint8_t)({src_r} >> {8 * (n - 1 - i)}));' for i in range(n)) + ' }'
        if dst_r and not src_r:          # d16(An) -> Dn
            ea = self._ea_addr(ops[0])
            val = ' | '.join(f'((uint32_t)bus_read8(_ea + {2 * i}) << {8 * (n - 1 - i)})' for i in range(n))
            if size == 16:
                return f'{{ uint32_t _ea = {ea}; {dst_r} = ({dst_r} & 0xFFFF0000u) | (uint16_t)({val}); }}'
            return f'{{ uint32_t _ea = {ea}; {dst_r} = {val}; }}'
        return None

    def _gen_chk(self, ops):
        """CHK <ea>,Dn: trap (vector 6) if Dn < 0 or Dn > bound."""
        d = self._reg(ops[1])
        if not d:
            return None
        bound = self._ea_read(ops[0], 16)
        # Z from Dn, V and C cleared whether it traps or not; N set when Dn < 0, cleared when
        # Dn > bound (the manual and the SingleStepTests suite; GPGX's core leaves N there)
        return (f'{{ int16_t _b = (int16_t)({bound}); int16_t _v = (int16_t){d}; '
                f'g_m68k.flag_Z = _v == 0; g_m68k.flag_V = g_m68k.flag_C = false; '
                f'if (_v < 0 || _v > _b) {{ g_m68k.flag_N = _v < 0; recomp_m68k_exception(6); }} '
                f'else g_cyc += 10 * 7; }}')        # in bounds: 10 clocks over the table's

    def _gen_link(self, ops):
        r = self._reg(ops[0])
        imm = self._imm(ops[1])
        if r and imm:
            return f'g_m68k.a[7] -= 4; bus_write32(g_m68k.a[7], {r}); {r} = g_m68k.a[7]; g_m68k.a[7] += (int16_t)({imm});'
        return None

    def _gen_unlk(self, ops):
        r = self._reg(ops[0])
        if r == 'g_m68k.a[7]':
            return 'g_m68k.a[7] = bus_read32(g_m68k.a[7]);'
        if r:
            return f'g_m68k.a[7] = {r}; {r} = bus_read32(g_m68k.a[7]); g_m68k.a[7] += 4;'
        return None

    def _gen_arith(self, op, ops, size):
        if len(ops) != 2:
            return None
        src = self._ea_read(ops[0], size)
        dst_r = self._reg(ops[1])
        if dst_r:
            return f'M68K_{op}{size}({dst_r}, {src});'
        # Memory destination: read-modify-write
        ea_addr = self._ea_addr(ops[1])
        return (f'{{ uint32_t _ea = {ea_addr}; uint{size}_t _tmp = bus_read{size}(_ea); '
                f'M68K_{op}{size}(_tmp, {src}); bus_write{size}(_ea, _tmp); }}')

    def _gen_adda(self, ops, size):
        src = self._ea_read(ops[0], size)
        r = self._reg(ops[1])
        if r:
            if size == 16:
                return f'{r} += (int16_t)(uint16_t)({src});'
            return f'{r} += {src};'
        return None

    def _gen_suba(self, ops, size):
        src = self._ea_read(ops[0], size)
        r = self._reg(ops[1])
        if r:
            if size == 16:
                return f'{r} -= (int16_t)(uint16_t)({src});'
            return f'{r} -= {src};'
        return None

    def _gen_addq(self, ops, size):
        imm = self._imm(ops[0])
        if not imm:
            return None
        dst_r = self._reg(ops[1])
        if dst_r:
            if ops[1].strip().lower().startswith('a'):
                return f'{dst_r} += {imm};'
            return f'M68K_ADD{size}({dst_r}, {imm});'
        # Memory destination
        ea_addr = self._ea_addr(ops[1])
        return (f'{{ uint32_t _ea = {ea_addr}; uint{size}_t _tmp = bus_read{size}(_ea); '
                f'M68K_ADD{size}(_tmp, {imm}); bus_write{size}(_ea, _tmp); }}')

    def _gen_subq(self, ops, size):
        imm = self._imm(ops[0])
        if not imm:
            return None
        dst_r = self._reg(ops[1])
        if dst_r:
            if ops[1].strip().lower().startswith('a'):
                return f'{dst_r} -= {imm};'
            return f'M68K_SUB{size}({dst_r}, {imm});'
        # Memory destination
        ea_addr = self._ea_addr(ops[1])
        return (f'{{ uint32_t _ea = {ea_addr}; uint{size}_t _tmp = bus_read{size}(_ea); '
                f'M68K_SUB{size}(_tmp, {imm}); bus_write{size}(_ea, _tmp); }}')

    def _gen_cmp(self, ops, size):
        src = self._ea_read(ops[0], size)
        dst = self._ea_read(ops[1], size)
        return f'M68K_CMP{size}({dst}, {src});'

    def _gen_cmpa(self, ops, size):
        src = self._ea_read(ops[0], size)
        r = self._reg(ops[1])
        if r:
            if size == 16:
                return f'M68K_CMP32({r}, (uint32_t)(int32_t)(int16_t)(uint16_t)({src}));'
            return f'M68K_CMP32({r}, {src});'
        return None

    def _gen_neg(self, op, ops, size):
        r = self._reg(ops[0])
        if r:
            return f'M68K_{op}{size}({r});'
        # Memory destination
        ea_addr = self._ea_addr(ops[0])
        return (f'{{ uint32_t _ea = {ea_addr}; uint{size}_t _tmp = bus_read{size}(_ea); '
                f'M68K_{op}{size}(_tmp); bus_write{size}(_ea, _tmp); }}')

    def _gen_mul(self, op, ops):
        src = self._ea_read(ops[0], 16)
        r = self._reg(ops[1])
        if r:
            return f'M68K_{op}({r}, {src});'
        return None

    def _gen_div(self, op, ops):
        src = self._ea_read(ops[0], 16)
        r = self._reg(ops[1])
        if r:
            return f'M68K_{op}({r}, {src});'
        return None

    def _gen_logic(self, op, ops, size):
        src = self._ea_read(ops[0], size)
        dst_r = self._reg(ops[1])
        if dst_r:
            return f'M68K_{op}{size}({dst_r}, {src});'
        # Memory destination
        ea_addr = self._ea_addr(ops[1])
        return (f'{{ uint32_t _ea = {ea_addr}; uint{size}_t _tmp = bus_read{size}(_ea); '
                f'M68K_{op}{size}(_tmp, {src}); bus_write{size}(_ea, _tmp); }}')

    def _gen_unary_logic(self, op, ops, size):
        r = self._reg(ops[0])
        if r:
            return f'M68K_{op}{size}({r});'
        # Memory destination
        ea_addr = self._ea_addr(ops[0])
        return (f'{{ uint32_t _ea = {ea_addr}; uint{size}_t _tmp = bus_read{size}(_ea); '
                f'M68K_{op}{size}(_tmp); bus_write{size}(_ea, _tmp); }}')

    def _gen_tst(self, ops, size):
        val = self._ea_read(ops[0], size)
        return f'M68K_TST{size}({val});'

    def _gen_btst(self, ops):
        bit = self._ea_read(ops[0], 32)
        r = self._reg(ops[1])
        if r:
            # Register: BTST operates on full 32 bits
            return f'M68K_BTST32({r}, {bit});'
        # Memory: BTST always operates on a byte regardless of Capstone suffix
        val = self._ea_read(ops[1], 8)
        return f'M68K_BTST8({val}, {bit});'

    def _gen_bset(self, ops):
        bit = self._ea_read(ops[0], 32)
        r = self._reg(ops[1])
        if r:
            return f'M68K_BSET32({r}, {bit});'
        # Memory: byte-size BSET
        ea_addr = self._ea_addr(ops[1])
        return (f'{{ uint32_t _ea = {ea_addr}; uint8_t _b = 1u << (({bit}) & 7); uint8_t _v = bus_read8(_ea); '
                f'g_m68k.flag_Z = !(_v & _b); bus_write8(_ea, _v | _b); }}')

    def _gen_bclr(self, ops):
        bit = self._ea_read(ops[0], 32)
        r = self._reg(ops[1])
        if r:
            return f'M68K_BCLR32({r}, {bit});'
        # Memory: byte-size BCLR
        ea_addr = self._ea_addr(ops[1])
        return (f'{{ uint32_t _ea = {ea_addr}; uint8_t _b = 1u << (({bit}) & 7); uint8_t _v = bus_read8(_ea); '
                f'g_m68k.flag_Z = !(_v & _b); bus_write8(_ea, _v & ~_b); }}')

    def _gen_bchg(self, ops):
        bit = self._ea_read(ops[0], 32)
        r = self._reg(ops[1])
        if r:
            return f'M68K_BCHG32({r}, {bit});'
        # Memory: byte-size BCHG
        ea_addr = self._ea_addr(ops[1])
        return (f'{{ uint32_t _ea = {ea_addr}; uint8_t _b = 1u << (({bit}) & 7); uint8_t _v = bus_read8(_ea); '
                f'g_m68k.flag_Z = !(_v & _b); bus_write8(_ea, _v ^ _b); }}')

    def _gen_shift(self, op, ops, size):
        if len(ops) == 2:
            cnt = self._ea_read(ops[0], 8)
            r = self._reg(ops[1])
            if r:
                return f'M68K_{op}{size}({r}, {cnt});'
            # Memory destination
            ea_addr = self._ea_addr(ops[1])
            return (f'{{ uint32_t _ea = {ea_addr}; uint{size}_t _tmp = bus_read{size}(_ea); '
                    f'M68K_{op}{size}(_tmp, {cnt}); bus_write{size}(_ea, _tmp); }}')
        elif len(ops) == 1:
            # Memory shift by 1 (e.g. LSL.W (An))
            ea_addr = self._ea_addr(ops[0])
            return (f'{{ uint32_t _ea = {ea_addr}; uint16_t _tmp = bus_read16(_ea); '
                    f'M68K_{op}16(_tmp, 1); bus_write16(_ea, _tmp); }}')
        return None

    def _is_local_target(self, target):
        """Check if target address is within current function."""
        return self.func_start <= target < self.func_end

    def _gen_bra(self, ops, addr):
        target = self._parse_branch_target(ops[0])
        if target is not None and self._is_local_target(target):
            label = self.labels.get(target, f'loc_{target:06X}')
            return f'goto {label};'
        if target is not None:
            return f'{{ func_table_call(0x{target:06X}); return; }}'
        return f'/* BRA unresolved */'

    def _gen_bsr(self, ops, addr):
        target = self._parse_branch_target(ops[0])
        label = self.labels.get(target, f'sub_{target:06X}')
        return f'func_table_jsr(0x{target:06X}, 0x{self._ret:06X}); /* {label} */'

    def _gen_jmp(self, ops, addr):
        target = self._parse_branch_target(ops[0])
        if target is not None:
            label = self.labels.get(target, f'sub_{target:06X}')
            if label.startswith('loc_'):
                return f'goto {label};'
            return f'{{ func_table_call(0x{target:06X}); return; }} /* {label} */'
        cases = getattr(self, 'pc_switches', {}).get(addr)
        if cases:
            # Compiled switch: every case is a label in this function
            ea_addr = self._ea_addr(ops[0])
            # Cases in this region are gotos; the rest become registered
            # entry points reached by tail jump (the region can end at the
            # JMP, leaving the case code in the next one).
            body = ' '.join(
                f'case 0x{t:06X}: goto {self.labels[t]};' if self._is_local_target(t)
                else f'case 0x{t:06X}: {{ func_table_tail(0x{t:06X}); return; }}'
                for t in sorted(set(cases)) if t < self.rom.size)
            return (f'switch ({ea_addr}) {{ {body} '
                    f'default: {{ func_table_call({ea_addr}); return; }} }} /* switch via {ops[0]} */')
        # True indirect JMP through register/EA
        ea_addr = self._ea_addr(ops[0])
        if 'UNHANDLED' not in ea_addr:
            return f'{{ func_table_call({ea_addr}); return; }} /* JMP indirect via {ops[0]} */'
        return f'/* JMP indirect: {ops[0]} — TODO */'

    def _gen_jsr(self, ops, addr):
        target = self._parse_branch_target(ops[0])
        if target is not None:
            label = self.labels.get(target, f'sub_{target:06X}')
            return f'func_table_jsr(0x{target:06X}, 0x{self._ret:06X}); /* {label} */'
        # True indirect JSR through register/EA: evaluate the EA before the push
        ea_addr = self._ea_addr(ops[0])
        if 'UNHANDLED' not in ea_addr:
            return f'{{ uint32_t _t = {ea_addr}; func_table_jsr(_t, 0x{self._ret:06X}); }} /* JSR indirect via {ops[0]} */'
        return f'/* JSR indirect: {ops[0]} — TODO */'

    def _gen_bcc(self, cc_macro, ops, addr):
        target = self._parse_branch_target(ops[0])
        if target is not None and self._is_local_target(target):
            label = self.labels.get(target, f'loc_{target:06X}')
            return f'if ({cc_macro}) goto {label};'
        if target is not None:
            return f'if ({cc_macro}) {{ func_table_call(0x{target:06X}); return; }}'
        return f'/* Bcc unresolved */'

    def _gen_scc(self, cc_macro, ops):
        r = self._reg(ops[0])
        if r:
            return f'{r} = ({r} & 0xFFFFFF00u) | ({cc_macro} ? 0xFFu : 0x00u);'
        return self._byte_store(ops[0], f'{cc_macro} ? 0xFF : 0x00')

    def _gen_dbcc(self, cc_macro, ops, addr):
        r = self._reg(ops[0])
        target = self._parse_branch_target(ops[1]) if len(ops) > 1 else None
        if r and target is not None:
            if self._is_local_target(target):
                label = self.labels.get(target, f'loc_{target:06X}')
                if cc_macro == '0':
                    return f'{{ int16_t _cnt = (int16_t)(uint16_t){r}; _cnt--; {r} = ({r} & 0xFFFF0000u) | (uint16_t)_cnt; if (_cnt != -1) goto {label}; }}'
                return f'if (!({cc_macro})) {{ int16_t _cnt = (int16_t)(uint16_t){r}; _cnt--; {r} = ({r} & 0xFFFF0000u) | (uint16_t)_cnt; if (_cnt != -1) goto {label}; }}'
            else:
                # Cross-function DBcc — emit as call
                if cc_macro == '0':
                    return f'{{ int16_t _cnt = (int16_t)(uint16_t){r}; _cnt--; {r} = ({r} & 0xFFFF0000u) | (uint16_t)_cnt; if (_cnt != -1) {{ func_table_call(0x{target:06X}); return; }} }}'
                return f'if (!({cc_macro})) {{ int16_t _cnt = (int16_t)(uint16_t){r}; _cnt--; {r} = ({r} & 0xFFFF0000u) | (uint16_t)_cnt; if (_cnt != -1) {{ func_table_call(0x{target:06X}); return; }} }}'
        return None

    def _gen_tas(self, ops):
        """TAS: N/Z from the byte, V/C cleared, bit 7 set. In memory the Mega Drive's bus never
        completes the write-back (Gargoyles and Ex-Mutants depend on it; GPGX disables it too)."""
        r = self._reg(ops[0])
        if r:
            return f'M68K_TST8((uint8_t){r}); {r} |= 0x80;'
        n = self._postinc(ops[0])
        if n is not None:
            return f'M68K_TST8(bus_read8(g_m68k.a[{n}])); g_m68k.a[{n}] += {self._step(n, 8)};'
        n = self._predec(ops[0])
        if n is not None:
            return f'g_m68k.a[{n}] -= {self._step(n, 8)}; M68K_TST8(bus_read8(g_m68k.a[{n}]));'
        return f'M68K_TST8(bus_read8({self._ea_addr(ops[0])}));'

    def _gen_st(self, ops):
        r = self._reg(ops[0])
        if r:
            return f'{r} = ({r} & 0xFFFFFF00u) | 0xFFu;'
        return self._byte_store(ops[0], '0xFF')

    def _gen_sf(self, ops):
        r = self._reg(ops[0])
        if r:
            return f'{r} = ({r} & 0xFFFFFF00u);'
        return self._byte_store(ops[0], '0x00')

    def _gen_move_sr(self, ops, mnemonic):
        if 'sr' in ops[0].lower():
            # MOVE SR, <ea>
            return self._ea_write(ops[1], 16, 'm68k_get_sr()')
        else:
            # MOVE <ea>, SR / MOVE <ea>, CCR (word source, only the low byte reaches the CCR;
            # the system byte - interrupt mask, S - is untouched)
            src = self._ea_read(ops[0], 16)
            if ops[1].strip().lower() == 'ccr':
                return f'm68k_set_ccr((uint8_t)({src}));'
            return f'm68k_set_sr({src});'

    def _gen_andi_sr(self, ops):
        imm = self._imm(ops[0])
        return f'm68k_set_sr(m68k_get_sr() & {imm});'

    def _gen_ori_sr(self, ops):
        imm = self._imm(ops[0])
        return f'm68k_set_sr(m68k_get_sr() | {imm});'

    def _parse_branch_target(self, op):
        op = op.strip()
        # $XXXX or $XXXX.l or $XXXX.w
        m = re.match(r'^\$([0-9A-Fa-f]+)(?:\.(w|l))?$', op)
        if m:
            try:
                v = int(m.group(1), 16)
            except ValueError:
                return None
            # Absolute short is sign-extended: $8000-$FFFF.w addresses RAM at $FF8000-$FFFFFF
            if m.group(2) == 'w' and 0x8000 <= v <= 0xFFFF:
                v |= 0xFF0000
            return v
        if op.startswith('0x'):
            try:
                return int(op.split('.')[0], 16)
            except ValueError:
                return None
        # Absolute with parens: ($XXXX).l
        m = re.match(r'^\(\$([0-9A-Fa-f]+)\)\.(w|l)$', op)
        if m:
            return int(m.group(1), 16)
        return None


# ============================================================
# Code generator — produces .c and .h files
# ============================================================

class CodeGenerator:
    MAX_FUNCS_PER_FILE = 50

    def __init__(self, rom, analyzer, translator, output_dir):
        self.rom = rom
        self.analyzer = analyzer
        self.translator = translator
        self.output_dir = output_dir
        self.cross_func_targets = set()  # addresses used by func_table_call
        self.resume_points = []  # (interior PC, owning function name)
        # Outputs are kept in memory and written at the end, each only if its text changed,
        # so a rebuild after adding a few functions recompiles only the chunks they touch
        self.outputs = {}
        os.makedirs(output_dir, exist_ok=True)

    CHUNK_SPAN = 0x2000   # chunks are ROM-address aligned: a new function changes only its own

    def _chunks(self, functions):
        """Split functions into files by 8 KB of ROM (more than MAX_FUNCS_PER_FILE: split
        inside the span). Names depend on the span, not on the functions around it."""
        spans = {}
        for f in functions:
            spans.setdefault(f['start'] // self.CHUNK_SPAN, []).append(f)
        chunks = []
        for span, funcs in sorted(spans.items()):
            for i in range(0, len(funcs), self.MAX_FUNCS_PER_FILE):
                chunks.append((f"recomp_{span * self.CHUNK_SPAN:06X}_{i // self.MAX_FUNCS_PER_FILE}",
                               funcs[i:i + self.MAX_FUNCS_PER_FILE]))
        return chunks

    def _hooks_for_interpreter(self):
        """The title hooks again, for code the runtime's interpreter runs (m68k_interp.c): each one
        a function returning where to go on, 0 = run the instruction; "goto skip;" -> the next
        instruction, "goto loc_/sub_XXXXXX;" -> that address. Without this, a hooked instruction
        reached in the interpreter (a function not converted, PS_INTERP) ran the original's logic."""
        hooks = getattr(self.translator, 'hooks', {}) or {}
        lens = getattr(self.translator, 'hook_len', {})
        out = ['/* generated: title hooks (recomp.json "hooks") for the interpreter, see generate.py */',
               '#include "recomp_common.h"', '']
        for a in sorted(hooks):
            body = re.sub(r'goto (?:loc|sub)_([0-9A-Fa-f]{6});', lambda m: f'return 0x{m.group(1).upper()}u;', hooks[a])
            nxt = a + lens[a] if a in lens else 1        # 1: skip of an instruction never converted
            body = body.replace('goto skip;', f'return 0x{nxt:06X}u;')
            out.append(f'static uint32_t hook_{a:06X}(void) {{ {body}\n    return 0; }}')
        pcs = ', '.join(f'0x{a:06X}u' for a in sorted(hooks)) or '0'
        fns = ', '.join(f'hook_{a:06X}' for a in sorted(hooks)) or '0'
        out += ['', f'static const uint32_t pcs[] = {{ {pcs} }};',
                f'static uint32_t (*const fns[])(void) = {{ {fns} }};',
                'const uint32_t *ps_m68k_hook_pc = pcs;', 'uint32_t (*const *ps_m68k_hook_fn)(void) = fns;',
                f'int ps_m68k_nhooks = {len(hooks)};', '']
        return '\n'.join(out)

    def _emit(self, name, text):
        self.outputs[name] = text

    def _flush(self):
        """Write changed outputs, drop generated files no longer produced."""
        written = 0
        for name, text in self.outputs.items():
            path = os.path.join(self.output_dir, name)
            try:
                with open(path) as f:
                    if f.read() == text:
                        continue
            except OSError:
                pass
            with open(path, 'w') as f:
                f.write(text)
            written += 1
        for f in os.listdir(self.output_dir):
            if f.startswith('recomp_') and f.endswith(('.c', '.h')) and f not in self.outputs:
                os.remove(os.path.join(self.output_dir, f))
        print(f"  {written} of {len(self.outputs)} files changed")

    def generate_all(self):
        """Generate all recompiled C files."""
        functions = sorted(self.analyzer.functions.values(), key=lambda f: f['start'])
        chunks = self._chunks(functions)

        # Generate each chunk (first pass — discovers cross-function targets)
        self.resume_points = []
        all_func_names = []
        for chunk_name, chunk_funcs in chunks:
            self._generate_chunk(chunk_name, chunk_funcs, functions)
            for f in chunk_funcs:
                all_func_names.append(f['name'])

        # Iteratively resolve cross-function call targets until stable.
        # Each pass may discover new targets from newly-split functions.
        prev = None
        for iteration in range(10):  # safety limit
            unregistered = self.cross_func_targets - set(f['start'] for f in functions)
            # Filter to valid ROM addresses
            unregistered = {a for a in unregistered if 0x200 <= a < self.rom.size and not (a & 1)}
            # the same targets left as after the last pass: splitting them made no new function
            # (functions are only added), so another pass would regenerate the same output
            if not unregistered or unregistered == prev:
                break
            prev = unregistered
            print(f"  Pass {iteration+1}: {len(unregistered)} cross-function targets — splitting and regenerating...")
            for addr in unregistered:
                if addr not in self.analyzer.labels:
                    self.analyzer.labels[addr] = f'loc_{addr:06X}'
            all_entries = set(f['start'] for f in functions) | unregistered
            self.analyzer._build_functions(all_entries)
            functions = sorted(self.analyzer.functions.values(), key=lambda f: f['start'])
            chunks = self._chunks(functions)
            self.outputs = {}
            self.cross_func_targets.clear()
            self.resume_points = []
            all_func_names = []
            for chunk_name, chunk_funcs in chunks:
                self._generate_chunk(chunk_name, chunk_funcs, functions)
                for f in chunk_funcs:
                    all_func_names.append(f['name'])
            print(f"    Now have {len(functions)} functions")

        # Generate registration header, then write what changed
        self._generate_registration(functions)
        # the cycle table for the runtime's interpreter (m68k_interp.c): the same costs as here
        rows = ',\n'.join(','.join(str(c) for c in CYCLES[i:i + 32]) for i in range(0, 0x10000, 32))
        self._emit('recomp_cycles.c', '/* generated: 68000 base cost per opcode (master clocks), as the generator charges */\n'
                   '#include <stdint.h>\nconst uint16_t ps_m68k_cycles[0x10000] = {\n' + rows + '\n};\n')
        self._emit('recomp_hooks.c', self._hooks_for_interpreter())
        self._flush()

        # Skip main.c generation — hand-written src/main.c is used instead
        # self._generate_main(functions)

        print(f"\nGenerated {len(chunks)} source files with {len(functions)} functions")
        print(f"Output directory: {self.output_dir}")

    def _generate_chunk(self, chunk_name, funcs, all_functions=None):
        """Generate one C source file for a chunk of functions."""
        lines = []
        lines.append(f'/* Generated by genrecomp tools/recompiler from {self.title}. Do not commit. */')
        lines.append(f'/* Source range: ${funcs[0]["start"]:06X} - ${funcs[-1]["end"]:06X} */')
        lines.append(f'/* Functions: {len(funcs)} */')
        lines.append(f'')
        lines.append(f'#include <genrecomp/genrecomp.h>')
        lines.append(f'#include "recomp_common.h"')
        lines.append(f'')
        decl_at = len(lines)   # this chunk's declarations go here (only what it calls)

        # Build a map from function start -> next function for fall-through
        next_func_map = {}
        if all_functions:
            for i in range(len(all_functions) - 1):
                next_func_map[all_functions[i]['start']] = all_functions[i + 1]

        regions = self._fallthrough_regions(all_functions or funcs)

        for func in funcs:
            region = regions[func['start']]
            next_func = next_func_map.get(region[-1]['start'])
            lines.extend(self._generate_function(func, next_func, region))
            lines.append('')

        names = {f['name'] for f in (all_functions or funcs)} | {'vec_chk'}
        used = sorted(set(re.findall(r'\b([A-Za-z_][A-Za-z0-9_]*)\s*\(', '\n'.join(lines))) & names)
        lines[decl_at:decl_at] = [f'void {n}(void);' for n in used] + ['']
        self._emit(f'{chunk_name}.c', '\n'.join(lines))

    @staticmethod
    def _fallthrough_regions(functions):
        """Group functions that run into each other into regions.

        The analyzer splits code at every externally-referenced address, so a
        loop whose head is such an address ends up spanning two C functions,
        and its back-edge becomes a recursive call that grows the C stack
        every iteration (see docs/debugging.md). Every function in a region
        is emitted with the whole region's code and enters it with a goto,
        so all branches inside the region stay gotos.
        """
        regions, cur = {}, []
        for f in functions:
            if cur:
                prev = cur[-1]
                last = prev['insn_addrs'][-1] if prev['insn_addrs'] else None
                mnem = CodeGenerator._mnem_of(prev, last)
                runs_on = (prev['end'] == f['start'] and not prev['has_return']
                           and not mnem.startswith(('bra', 'jmp')))
                if not runs_on:
                    for g in cur:
                        regions[g['start']] = cur
                    cur = []
            cur.append(f)
        for g in cur:
            regions[g['start']] = cur
        return regions

    _instructions = None

    @staticmethod
    def _mnem_of(func, addr):
        insns = CodeGenerator._instructions
        return insns[addr][0] if insns and addr in insns else ''

    def _generate_function(self, func, next_func=None, region=None):
        """Generate C code for one function (entered at func['start'])."""
        CodeGenerator._instructions = self.analyzer.instructions
        region = region or [func]
        lines = []
        name = func['name']
        start = func['start']
        end = func['end']
        reg_start, reg_end = region[0]['start'], region[-1]['end']
        insn_addrs = [a for g in region for a in g.get('insn_addrs', [])]

        lines.append(f'/* ${start:06X}-${end:06X}  ({func["insn_count"]} instructions, {func["size"]} bytes) */')
        if len(region) > 1:
            lines.append(f'/* region ${reg_start:06X}-${reg_end:06X}, entered at ${start:06X} */')
        lines.append(f'void {name}(void) {{')

        if not insn_addrs:
            lines.append(f'    /* empty function */')
            lines.append(f'}}')
            return lines

        # Every function start is a potential goto target within the region
        for g in region:
            self.analyzer.labels.setdefault(g['start'], g['name'])

        # Resumable interior entries: the instruction after every JSR/BSR is
        # where an RTS lands. Code that rewrites or drops return addresses
        # (task switchers, object loops) can RTS there from a different C
        # frame; a resume wrapper re-enters this function at that PC.
        insn_set = set(insn_addrs)
        resume = []
        for a in insn_addrs:
            ins = self.analyzer.instructions.get(a)
            if ins and ins[0] in ('jsr', 'bsr', 'bsr.s', 'bsr.w', 'bsr.b'):
                r = a + ins[2]
                if r in insn_set and r != start:
                    resume.append(r)
                    self.analyzer.labels.setdefault(r, f'loc_{r:06X}')
        if resume:
            lines.append('    if (g_func_resume_pc) {')
            lines.append('        uint32_t _r = g_func_resume_pc; g_func_resume_pc = 0;')
            lines.append('        switch (_r) {')
            for r in resume:
                lines.append(f'        case 0x{r:06X}: goto {self.analyzer.labels[r]};')
            lines.append('        }')
            lines.append('    }')
        self.resume_points.extend((r, name) for r in resume)
        if start != reg_start:
            lines.append(f'    goto {self.analyzer.labels[start]};')

        for addr in insn_addrs:
            if addr not in self.analyzer.instructions:
                continue
            mnemonic, op_str, size, raw = self.analyzer.instructions[addr]
            c_lines = self.translator.translate_instruction(addr, mnemonic, op_str, raw, reg_start, reg_end)
            lines.extend(c_lines)

        # If function didn't end with RTS/RTE/JMP/BRA, it falls through to the
        # next function. Generate a call to the next function so execution continues.
        # Skip if the last emitted line already returns (e.g. func_table_call + return).
        func = region[-1]
        if not func['has_return'] and next_func:
            # Check if the last line is an unconditional return/jump.
            # Conditional returns (inside if) still need fall-through.
            last_code = lines[-1].strip() if lines else ''
            needs_fallthrough = True
            if last_code == 'return;' or last_code.endswith('/* RTE */'):
                needs_fallthrough = False
            elif '{ func_table_call(' in last_code and 'return; }' in last_code and not last_code.startswith('if'):
                needs_fallthrough = False
            if needs_fallthrough:
                # Fall through to the byte after the last instruction, not to the next
                # function's start: that one can begin INSIDE the last instruction (a jump-table
                # target landing mid-instruction). SH1 $B110 move.b #0,$34(a0) (6 bytes)
                # fell into loc_00B114 (target of the $AEC6 table) and ran "ori.b #$75,$20(a4,d2.w)"
                # instead of the rts at $B116: $FFF786 = $75, RAM diverged on every long play.
                last = [a for a in insn_addrs if a in self.analyzer.instructions][-1]
                ft = last + self.analyzer.instructions[last][2]
                next_name = next_func['name'] if ft == next_func['start'] else f'${ft:06X}, next function {next_func["name"]}'
                lines.append(f'    /* Fall through to next function */')
                lines.append(f'    func_table_tail(0x{ft:06X}); /* {next_name} */')

        lines.append(f'}}')

        # Post-process: fix cross-function gotos
        # Collect all labels defined in this function
        defined_labels = set()
        for line in lines:
            stripped = line.strip()
            if stripped.endswith(':') and not stripped.startswith('/*') and not stripped.startswith('//'):
                label_name = stripped[:-1]
                defined_labels.add(label_name)

        # Replace gotos to undefined labels with func_table_call
        fixed_lines = []
        for line in lines:
            m = re.search(r'goto ((?:loc|sub|jt|vec|main_game_entry)_?([0-9A-Fa-f]+));', line)
            if m and m.group(1) not in defined_labels:
                addr_val = int(m.group(2), 16)
                self.cross_func_targets.add(addr_val)
                line = line.replace(
                    f'goto {m.group(1)};',
                    f'{{ func_table_call(0x{addr_val:06X}); return; }}'
                )
            fixed_lines.append(line)

        # Jumps out of the function are tail calls: hand them to the
        # dispatcher's trampoline so M68K loops through JMP/BRA chains don't
        # grow the C stack (genrecomp func_table_tail).
        fixed_lines = [re.sub(r'func_table_call\(([^;]*)\); return;', r'func_table_tail(\1); return;', l)
                       for l in fixed_lines]
        # A JSR whose callee returned straight to an outer frame must return too
        fixed_lines = [re.sub(r'(?<!!)func_table_jsr\(([^;]*)\);', r'if (!func_table_jsr(\1)) return;', l)
                       for l in fixed_lines]

        # Drop labels nothing jumps to (every instruction address with a
        # label gets one, which would otherwise trip unused-label warnings)
        used = set(re.findall(r'goto (\w+);', '\n'.join(fixed_lines)))
        fixed_lines = [l for l in fixed_lines
                       if not (re.fullmatch(r'\w+:', l.strip()) and l.strip()[:-1] not in used)]

        # Also track all func_table_call targets already in the code
        # (from translator's _gen_bsr, _gen_bcc, etc.)
        for line in fixed_lines:
            for m in re.finditer(r'func_table_(?:call|tail)\(0x([0-9A-Fa-f]+)\)', line):
                self.cross_func_targets.add(int(m.group(1), 16))

        return fixed_lines

    def _generate_registration(self, functions):
        """Generate the header with forward declarations and registration function."""
        lines = []
        lines.append('/* Auto-generated — forward declarations + registration */')
        lines.append('#ifndef RECOMP_FUNCS_H')
        lines.append('#define RECOMP_FUNCS_H')
        lines.append('')
        lines.append('#include "recomp_common.h"')
        lines.append('')
        common = ['/* Auto-generated: helpers every chunk uses (stable: chunks do not depend on the',
                  ' * function list, so adding functions recompiles only the chunks that change) */',
                  '#ifndef RECOMP_COMMON_H', '#define RECOMP_COMMON_H', '', '#include <genrecomp/genrecomp.h>', '']
        # MSVC-compatible helpers for post-increment/pre-decrement addressing modes
        common.append('/* Post-increment read: read from (An) then An += size */')
        for sz in [8, 16, 32]:
            inc = sz // 8
            step = '(n == 7 ? 2 : 1)' if sz == 8 else str(inc)   # byte (A7)+ moves SP by 2
            common.append(f'static inline uint{sz}_t _postinc{sz}(int n) {{ uint{sz}_t v = bus_read{sz}(g_m68k.a[n]); g_m68k.a[n] += {step}; return v; }}')
        common.append('/* Pre-decrement read: An -= size then read from (An) */')
        for sz in [8, 16, 32]:
            dec = sz // 8
            step = '(n == 7 ? 2 : 1)' if sz == 8 else str(dec)   # byte -(A7) moves SP by 2
            common.append(f'static inline uint{sz}_t _predec{sz}(int n) {{ g_m68k.a[n] -= {step}; return bus_read{sz}(g_m68k.a[n]); }}')
        common += ['', '#endif /* RECOMP_COMMON_H */', '']
        self._emit('recomp_common.h', '\n'.join(common))

        # Forward declarations
        for func in functions:
            lines.append(f'void {func["name"]}(void);')
        lines.append('')

        # Resume wrappers (interior PCs an RTS may land on), unless the PC is
        # already a function entry of its own
        starts = set(f['start'] for f in functions)
        resume = {}
        for r, owner in self.resume_points:
            if r not in starts and r not in resume:
                resume[r] = owner
        for r in sorted(resume):
            lines.append(f'void res_{r:06X}(void);')
        self._emit('recomp_resume.c', '/* Auto-generated — resumable interior entry points */\n'
                   '#include "recomp_funcs.h"\n\n'
                   + ''.join(f'void res_{r:06X}(void) {{ g_func_resume_pc = 0x{r:06X}; {resume[r]}(); }}\n'
                             for r in sorted(resume)))
        lines.append('')

        # Registration function
        lines.append('static inline void recomp_register_all(void) {')
        for func in functions:
            lines.append(f'    func_table_register(0x{func["start"]:06X}, {func["name"]});')
        for r in sorted(resume):
            lines.append(f'    func_table_register(0x{r:06X}, res_{r:06X});')
        lines.append('}')
        lines.append('')
        lines.append('#endif /* RECOMP_FUNCS_H */')

        self._emit('recomp_funcs.h', '\n'.join(lines))


def main():
    import argparse
    parser = argparse.ArgumentParser(description='genrecomp 68K recompiler')
    parser.add_argument('rom', help='Path to Genesis ROM file')
    parser.add_argument('--output-dir', '-o', default='src/recomp', help='Output directory for generated C files')
    parser.add_argument('--config', '-c', help='Title config JSON (title, seeds, code_ranges)')
    args = parser.parse_args()

    import json
    cfg = json.load(open(args.config)) if args.config else {}
    print(f"=== genrecomp recompiler: {cfg.get('title', args.rom)} ===\n")

    rom = GenesisROM(args.rom)
    rom.print_info()

    # Cheats (recomp.json "cheats": name -> {address: replacement hex}): analysis runs on the
    # patched image so newly reachable code is compiled; each patched instruction is emitted
    # as `if (ps_cheat(n)) patched else original`, switchable at runtime.
    cheats = cfg.get('cheats') or {}
    orig_data = rom.data
    cheat_sites = {}
    if cheats:
        patched = bytearray(rom.data)
        for n, (name, sites) in enumerate(cheats.items()):
            for a, hexv in sites.items():
                a = int(a, 16); b = bytes.fromhex(hexv)
                patched[a:a + len(b)] = b
                cheat_sites[a] = n
        rom.data = bytes(patched)
        # the cheat-off path must be compiled too: the original instruction's successors (its
        # fall-through, its branch target) are entry points, since analysing the patched image
        # alone never reaches code only the original goes to (WOI's invincibility
        # rts at $F030 hid the bne's fall-through $F032)
        md = Cs(CS_ARCH_M68K, CS_MODE_M68K_000)
        seeds = cfg.setdefault('seeds', {})
        for a in cheat_sites:
            o = next(md.disasm(orig_data[a:a + 10], a), None)
            if not o:
                continue
            succ = [] if o.mnemonic in ('rts', 'rte', 'rtr', 'jmp', 'bra', 'bra.s', 'bra.w', 'bra.b') else [a + o.size]
            m = re.match(r'\$([0-9a-fA-F]+)$', o.op_str.strip())
            if o.mnemonic.split('.')[0] in ('bra', 'bsr') or (o.mnemonic[0] == 'b' and m):
                if m:
                    succ.append(int(m.group(1), 16))
            for s in succ:
                seeds.setdefault(f'0x{s:06X}', f'sub_{s:06X}')

    print("\n--- Phase 1: Analysis ---")
    analyzer = M68KAnalyzer(rom)
    analyzer.load_config(cfg)
    analyzer.analyze()

    # Phase 1.5: discover additional entry points from cross-function calls
    # that reference addresses not in any discovered function
    extra_entries = set()
    for func in analyzer.functions.values():
        for call_addr in func['calls']:
            if call_addr not in analyzer.functions and 0x200 <= call_addr < rom.size:
                extra_entries.add(call_addr)
    # Also scan generated code patterns for func_table_call references
    for addr, (mnemonic, op_str, size, raw) in analyzer.instructions.items():
        if mnemonic in ('jmp', 'jsr', 'bra', 'bsr'):
            target = analyzer._extract_branch_target(
                type('obj', (), {'op_str': op_str, 'operands': [], 'mnemonic': mnemonic})(),
                addr
            )
            if target and target not in analyzer.functions and 0x200 <= target < rom.size and not (target & 1):
                extra_entries.add(target)

    if extra_entries:
        print(f"\n--- Phase 1.5: Discovering {len(extra_entries)} additional functions ---")
        work = list(extra_entries)
        while work:
            new_work = []
            for addr in work:
                if addr in analyzer.visited or addr >= rom.size or addr < 0x200 or (addr & 1):
                    continue
                new_targets = analyzer._disassemble_block(addr)
                for target, is_call in new_targets:
                    if target not in analyzer.visited and 0x200 <= target < rom.size and not (target & 1):
                        new_work.append(target)
                        if is_call:
                            extra_entries.add(target)
            work = new_work
        analyzer._build_functions(extra_entries | set(analyzer.functions.keys()))
        print(f"Now have {len(analyzer.functions)} functions, {len(analyzer.instructions)} instructions")

    print("\n--- Phase 2: Code Generation ---")
    translator = M68KTranslator(rom, analyzer.labels)
    translator.hooks = {int(k, 16): v for k, v in (cfg.get('hooks') or {}).items()}
    translator.cheat_sites = cheat_sites
    translator.orig_data = orig_data
    translator.cheat_names = list(cheats.keys())
    translator.pc_switches = analyzer.pc_switches
    generator = CodeGenerator(rom, analyzer, translator, args.output_dir)
    generator.title = cfg.get('title', rom.title_overseas or 'a Genesis ROM')
    generator.generate_all()
    # cheat names for the runtime / settings menu (index = ps_cheat argument)
    with open(os.path.join(args.output_dir, 'recomp_cheats.c'), 'w') as cf:
        names = list(cheats.keys())
        cf.write('/* generated: cheats from recomp.json */\n')
        cf.write(f'const char *ps_cheat_names[] = {{ {", ".join(chr(34) + n + chr(34) for n in names) or "0"} }};\n')
        cf.write(f'int ps_cheat_count = {len(names)};\n')

    print("\nDone!")


if __name__ == '__main__':
    main()
