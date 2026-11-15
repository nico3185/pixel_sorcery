| SH2 on the Mega CD compilation, one image per zone (sh2_disc.sh): every image is packed on its
| own (its code and data where zonepack.py puts them), so nothing but the game's own state survives a
| switch; this helper sits at the same address in all of them ($200510, after the image's descriptor at
| $200500). Two entries:
|   +0  the main loop's dispatch (ROM $394: move.b/andi/jsr patched to jsr here): a game mode this image
|       holds runs from the image's own mode table (TABLE, ROM $3A2 relocated); another one switches to
|       the image MODES names for it; $FF there = a level mode in an image without levels (the special
|       stage's): the zone decides, as below
|   +4  the Level routine's entry (ROM $3EC4, its first instruction patched to jmp here): a zone this
|       image holds loads as before (LEVELREST, ROM $3ECA relocated); another zone switches to the image
|       ZONES names for it
|   +8  see below
| MODES (11 bytes, game mode / 4) and ZONES (17 bytes, zone id) hold 0 = here, else $80 + group; they
| come from sh2_disc.sh as tables.inc. A switch copies the stub (swap.s) below the stack and runs it
| with d7 = $80 + group; the new image starts at its main loop, which reads the game mode.
	.text
	.globl	_start
_start:
	bra.w	dispatch
	bra.w	level
	jsr	FAR1                            | +8: Level's bsr.w $BFBC / bsr.w $4F52 (ROM $411A, patched to jsr
	jmp	FAR2                            |  here + nop): the only tie between the code below $5000 and the
	                                        |  rest - with it the piece is 162 KB, word RAM has 157 in a row
dispatch:
	moveq	#0, %d0
	move.b	0xFFFFF600, %d0
	andi.w	#0x3C, %d0
	move.w	%d0, %d1
	lsr.w	#2, %d1
	lea	modes(%pc), %a0
	move.b	(%a0, %d1.w), %d7
	beq.s	1f
	cmpi.b	#0xFF, %d7
	bne.s	switch
	bsr.s	zone                            | a level mode, no level here: the zone's image
	bra.s	switch
1:	lea	TABLE, %a0
	jmp	(%a0, %d0.w)
level:
	bsr.s	zone
	bne.s	switch
	bset	#7, 0xFFFFF600                  | Level's own first instruction
	jmp	LEVELREST
zone:	moveq	#0, %d0
	move.b	0xFFFFFE10, %d0
	cmpi.b	#0x10, %d0
	bls.s	2f
	moveq	#0, %d0
2:	lea	zones(%pc), %a0
	move.b	(%a0, %d0.w), %d7
	rts
switch:
	move.w	#0x2700, %sr
	lea	0xFFFE00, %sp                   | Level is reached with a deep stack: it would run over the
	                                        | copied stub - nothing on it has to survive the switch
	lea	stub(%pc), %a0
	lea	0xFFFD20, %a1
	move.w	#(stubend - stub) / 2 - 1, %d0
3:	move.w	(%a0)+, (%a1)+
	dbra	%d0, 3b
	jmp	0xFFFD20
	.include "tables.inc"
	.even
stub:	.incbin	"swap.bin"
	.even
stubend:
