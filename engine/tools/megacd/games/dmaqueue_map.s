| SH-engine DMA queue run for the Mega CD disc (SH2, SH3, SHK; relocate.py --blob or included,
| linked at its address). The game's Process_DMA_Queue is patched to jump here.
| --defsym QBASE / QEND: the queue's low word address and end slot (SH2 $DC00/$DCFC, SH3 $FB00/$FBFC);
| ART2LO-ART2HI, ART3LO-ART3HI: the cartridge ranges of the art copied to program-RAM banks 2 and 3 (an
| unused one: 1, 1). Each source moves to where the main CPU sees it on the disc: that art is read through the
| $020000 window with its bank selected - the games build those sources from offsets, so they arrive as
| cartridge addresses (a DMA from them reads the BIOS ROM's mirror), or as the virtual $0A0000/$0E0000 where a
| reference was relocated. A source in Word RAM ($200000-$23FFFF) reaches the VDP one word late (the first
| word written is stale): it starts 2 bytes on and its first word is written by hand after. Anything else is
| sent as queued; bank 1 is selected again at the end.
	.text
	.globl	dmaqueue
dmaqueue:
	movem.l	%d1-%d6/%a2, -(%sp)
	lea	0xC00004, %a5
	movea.w	#QBASE, %a1                     | $FFxxxx, as the original's .w
1:	tst.w	(%a1)
	beq.w	9f
| fast path (SH3, SHK: their art ranges are whole 128 KB blocks): a source in one is the same offset in the
| $020000 window - only its $97 byte changes - as cheap as the cartridge's own run (VBlank has no time to spare:
| Angel Island 1 lost a frame to the general path)
	.if	((ART2LO & 0x1FFFF) == 0) && (ART2HI - ART2LO == 0x20000)
	move.b	9(%a1), %d1
	andi.b	#0x7F, %d1
	cmpi.b	#ART2LO >> 17, %d1
	bne.s	10f
	move.b	#0x80, 0xA12003
	bra.s	12f
10:
	.if	((ART3LO & 0x1FFFF) == 0) && (ART3HI - ART3LO == 0x20000)
	cmpi.b	#ART3LO >> 17, %d1
	bne.s	11f
	move.b	#0xC0, 0xA12003
	.else
	bra.s	11f
	.endif
12:	move.w	(%a1)+, (%a5)                   | length, source low and mid as queued
	move.w	(%a1)+, (%a5)
	move.w	(%a1)+, (%a5)
	move.w	(%a1)+, (%a5)
	move.w	(%a1)+, %d3                     | $97: the window's block ($020000 >> 17 = 1)
	move.b	#1, %d3
	move.w	%d3, (%a5)
	move.w	(%a1)+, (%a5)                   | the command: starts the transfer
	move.w	(%a1)+, (%a5)
13:	move.w	(%a5), %d3
	btst	#1, %d3
	bne.s	13b
	cmpa.w	#QEND, %a1
	bne.w	1b
	bra.w	9f
11:
	.endif
	moveq	#0, %d1                         | the source's byte address, from $97/$96/$95
	move.b	9(%a1), %d1
	andi.b	#0x7F, %d1
	lsl.l	#8, %d1
	move.b	7(%a1), %d1
	lsl.l	#8, %d1
	move.b	5(%a1), %d1
	add.l	%d1, %d1
	moveq	#0x40, %d2                      | bank 1
	moveq	#0, %d4                         | no Word-RAM fix
	lea	ranges(%pc), %a2
2:	move.l	(%a2)+, %d3                     | low bound (0: end of the list)
	beq.s	5f
	move.l	(%a2)+, %d5                     | high bound
	move.w	(%a2)+, %d6                     | bank, or 0 for Word RAM
	cmp.l	%d3, %d1
	blo.s	2b
	cmp.l	%d5, %d1
	bhs.s	2b
	tst.w	%d6
	beq.s	3f
	sub.l	%d3, %d1                        | into the $020000 window
	addi.l	#0x20000, %d1
	move.b	%d6, %d2
	bra.s	5f
3:	movea.l	%d1, %a2                        | Word RAM: the first word, by hand after
	addq.l	#2, %d1
	moveq	#1, %d4
5:	move.b	%d2, 0xA12003
	move.w	(%a1)+, (%a5)                   | length
	move.w	(%a1)+, (%a5)
	lsr.l	#1, %d1
	move.w	#0x9500, %d3
	move.b	%d1, %d3
	move.w	%d3, (%a5)
	lsr.l	#8, %d1
	move.w	#0x9600, %d3
	move.b	%d1, %d3
	move.w	%d3, (%a5)
	lsr.l	#8, %d1
	move.w	4(%a1), %d3                     | $97: its mode bit kept
	andi.w	#0xFF80, %d3
	andi.b	#0x7F, %d1
	or.b	%d1, %d3
	move.w	%d3, (%a5)
	lea	6(%a1), %a1
	move.w	(%a1)+, %d5                     | the command: starts the transfer
	move.w	(%a1)+, %d6
	move.w	%d5, (%a5)
	move.w	%d6, (%a5)
4:	move.w	(%a5), %d3                      | wait for the transfer to finish (DMA busy, status
	btst	#1, %d3                         |  bit 1) before the bank changes: an emulator may
	bne.s	4b                              |  read a long transfer's source over several lines
	tst.w	%d4
	beq.s	6f
	andi.w	#0xFF7F, %d6                    | the same address, a plain write: the first word
	move.w	%d5, (%a5)
	move.w	%d6, (%a5)
	move.w	(%a2), -4(%a5)
6:	cmpa.w	#QEND, %a1
	bne.w	1b
9:	move.b	#0x40, 0xA12003
	move.w	#0, 0xFFFF0000 + QBASE
	move.l	#0xFFFF0000 + QBASE, 0xFFFF0000 + QEND
	movem.l	(%sp)+, %d1-%d6/%a2
	rts
ranges:	.long	ART2LO, ART2HI
	.word	0x80                            | the art in bank 2 (cartridge addresses)
	.long	ART3LO, ART3HI
	.word	0xC0                            | the art in bank 3
	.long	0x0A0000, 0x0C0000
	.word	0x80                            | the same, relocated references
	.long	0x0E0000, 0x100000
	.word	0xC0
	.long	0x200000, 0x240000
	.word	0                               | Word RAM
	.long	0
