| SH2 on Mega CD: helpers placed by relocate.py --blob (linked at their address).
| --defsym SNDLOAD = the relocated sound-driver loader (ROM $EC000)
| --defsym ZB1D, ZB1E, ZB1F = new Z80 bank numbers for ROM banks $E8000, $F0000, $F8000
|
| sndload ($130A's jmp is patched to here): the driver is unpacked into Z80 RAM by the game's
| loader; its bank selects are constant 9-bit sequences (one "ld (hl),e" = 1 or "ld (hl),a" = 0
| per address line, A15 first) at fixed driver offsets - rewritten for the banks' new home.
|
| dmaqueue (Process_DMA_Queue, $14AC, patched to jump here): the same queue run, selecting the
| program-RAM bank each transfer reads from. Sonic's art sits at virtual $0A0000 (bank 2), Tails'
| at $0E0000 (bank 3): mirrors of the $020000 window, told apart by the source's bits 17-23
| (the $97xx register word). Anything else: bank 1, which is also restored at the end.
	.text
	.globl	_start
_start:
sndload:
	jsr	SNDLOAD
	move.w	#0x100, 0xA11100                | Z80 bus
1:	btst	#0, 0xA11100
	bne.s	1b
	lea	0xA00000, %a0
	lea	seqs(%pc), %a1
	moveq	#(seqend - seqs) / 4 - 1, %d0
2:	move.w	(%a1)+, %d1                     | driver offset of the sequence
	move.w	(%a1)+, %d2                     | bank number
	moveq	#8, %d3
3:	moveq	#0x77, %d4                      | ld (hl),a: 0
	lsr.w	#1, %d2
	bcc.s	4f
	moveq	#0x73, %d4                      | ld (hl),e: 1
4:	move.b	%d4, (%a0, %d1.w)
	addq.w	#1, %d1
	dbra	%d3, 3b
	dbra	%d0, 2b
	move.w	#0, 0xA11100
	rts
seqs:	.word	0x097 + 3, ZB1F
	.word	0x0CF + 3, ZB1D
	.word	0x629 + 3, ZB1F
	.word	0x6F7 + 3, ZB1E
	.word	0x978 + 3, ZB1F
	.word	0xC6C + 3, ZB1E
	.word	0xC7C + 3, ZB1F
	.word	0xF3C + 3, ZB1F
seqend:

dmaqueue:
	lea	0xC00004, %a5
	movea.w	#0xDC00, %a1                     | $FFFFDC00, as the original's .w
1:	move.w	(%a1)+, %d0
	beq.s	9f
	move.b	7(%a1), %d1                     | source bits 17-23
	moveq	#0x40, %d2                      | bank 1
	cmp.b	#5, %d1
	bne.s	2f
	move.b	#0x80, %d2                      | bank 2: Sonic's art
2:	cmp.b	#7, %d1
	bne.s	3f
	move.b	#0xC0, %d2                      | bank 3: Tails' art
3:	move.b	%d2, 0xA12003
	move.w	%d0, (%a5)
	move.w	(%a1)+, (%a5)
	move.w	(%a1)+, (%a5)
	move.w	(%a1)+, (%a5)
	move.w	(%a1)+, (%a5)
	move.w	(%a1)+, (%a5)
	move.w	(%a1)+, (%a5)
4:	move.w	(%a5), %d3                      | wait for the transfer to finish (DMA busy, status
	btst	#1, %d3                         |  bit 1) before the bank changes: an emulator may
	bne.s	4b                              |  read a long transfer's source over several lines
	cmpa.w	#0xDCFC, %a1
	bne.s	1b
9:	move.b	#0x40, 0xA12003
	move.w	#0, 0xFFDC00
	move.l	#0xFFFFDC00, 0xFFDCFC
	rts
