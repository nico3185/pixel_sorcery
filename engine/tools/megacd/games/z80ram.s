| Sound driver bank patches applied in Z80 RAM (Mega CD; relocate.py --blob, linked at its
| address): the game's driver loader (--defsym SNDLOAD, relocated) is patched to jump here; this
| runs it, then takes the Z80 bus and writes the bytes of z80patch.bin (pairs: Z80 RAM offset
| word, value word; end $FFFF) - the bank numbers z80plan_ram.py moved to word RAM.
	.text
	.globl	_start
_start:
	move.w	#0x100, 0xA11100                | the loader's first instruction (replaced by our jmp)
	jsr	SNDLOAD
	move.w	#0x100, 0xA11100                | Z80 bus
1:	btst	#0, 0xA11100
	bne.s	1b
	lea	0xA00000, %a0
	lea	pairs(%pc), %a1
2:	move.w	(%a1)+, %d0
	cmp.w	#0xFFFF, %d0
	beq.s	3f
	move.w	(%a1)+, %d1
	move.b	%d1, (%a0, %d0.w)
	bra.s	2b
3:	move.w	#0, 0xA11100
	rts
	.even
pairs:	.incbin	"z80patch.bin"
	.word	0xFFFF
