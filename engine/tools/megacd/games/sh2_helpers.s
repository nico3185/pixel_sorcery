| SH2 on Mega CD: helpers placed by relocate.py --blob (linked at their address).
| --defsym SNDLOAD = the relocated sound-driver loader (ROM $EC000)
| --defsym ZB1D, ZB1E, ZB1F = new Z80 bank numbers for ROM banks $E8000, $F0000, $F8000
|
| sndload ($130A's jmp is patched to here): the driver is unpacked into Z80 RAM by the game's
| loader; its bank selects are constant 9-bit sequences (one "ld (hl),e" = 1 or "ld (hl),a" = 0
| per address line, A15 first) at fixed driver offsets - rewritten for the banks' new home.
|
| dmaqueue (Process_DMA_Queue, $14AC, patched to jump here): dmaqueue_map.s, included below.
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

	.equ	QBASE, 0xDC00
	.equ	QEND, 0xDCFC
	.equ	ART2LO, 0x050000                | Sonic's art
	.equ	ART2HI, 0x064320
	.equ	ART3LO, 0x064320                | Tails'
	.equ	ART3HI, 0x06FBE0
	.include "dmaqueue_map.s"
