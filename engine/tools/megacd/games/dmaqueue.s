| SH-engine DMA queue run with program-RAM bank selection (Mega CD; relocate.py --blob, linked
| at its address). The game's Process_DMA_Queue is patched to jump here.
| --defsym QBASE = the queue's low word address (SH2 $DC00, SH3 $FB00), QEND its end slot
| ($DCFC, $FBFC). Data only DMA reads lives in program-RAM banks 2/3 at virtual $0A0000/$0E0000
| (mirrors of the $020000 window, told apart by the source's bits 17-23 in the $97xx word); each
| transfer selects its bank, bank 1 (the CPU's) is restored at the end.
	.text
	.globl	_start
_start:
dmaqueue:
	lea	0xC00004, %a5
	movea.w	#QBASE, %a1                     | $FFxxxx, as the original's .w
1:	move.w	(%a1)+, %d0
	beq.s	9f
	move.b	7(%a1), %d1                     | source bits 17-23
	moveq	#0x40, %d2                      | bank 1
	cmp.b	#5, %d1
	bne.s	2f
	move.b	#0x80, %d2                      | bank 2
2:	cmp.b	#7, %d1
	bne.s	3f
	move.b	#0xC0, %d2                      | bank 3
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
	cmpa.w	#QEND, %a1
	bne.s	1b
9:	move.b	#0x40, 0xA12003
	move.w	#0, 0xFFFF0000 + QBASE
	move.l	#0xFFFF0000 + QBASE, 0xFFFF0000 + QEND
	rts
