| SH3 on the Mega CD compilation, one image per zone or act (sh3_disc.sh) - sh2_zone.s's scheme: every image is packed on its
| own (its code and data where zonepack.py puts them), so nothing but the game's own state survives a
| switch; this helper sits at the same address in all of them ($200510, after the image's descriptor at
| $200500). Two entries:
|   +0  the main loop's dispatch (ROM $734: move.b/andi/movea/jsr patched to jsr here): a game mode this
|       image holds runs from the image's own mode table (TABLE, ROM $744 relocated: longs); another one switches to
|       the image MODES names for it; $FF there = a level mode in an image without levels (the special
|       stage's): the zone decides, as below
|   +4  the Level routine's entry (ROM $46B0, its first instruction patched to jmp here): a zone - and act,
|       ACTS (48 bytes: zone x 2 + act, zones $00-$17) - this image holds loads as before (LEVELREST, ROM $46B6 relocated);
|       another switches to the image ACTS names for it
| MODES (32 bytes, game mode / 4) and ACTS hold 0 = here, else $80 + group; they come from
| sh3_disc.sh as tables.inc. A switch copies the stub (swap.s) below the stack and runs it
| with d7 = $80 + group; the new image starts at its main loop, which reads the game mode.
	.text
	.globl	_start
_start:
	bra.w	dispatch
	bra.w	level
dispatch:
	moveq	#0, %d0
	move.b	0xFFFFF600, %d0
	andi.w	#0x7C, %d0
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
	movea.l	(%a0, %d0.w), %a0
	jmp	(%a0)
level:
	bsr.s	zone
	bne.s	5f
	bset	#7, 0xFFFFF600                  | Level's own first instruction
	jmp	LEVELREST
5:	move.w	#0x2700, %sr                    | another image's zone: let one frame pass first (interrupts off, the
6:	btst	#3, 0xC00005                    |  VDP's VBlank flag: out of it, then into it), then read the zone
	bne.s	6b                              |  again - a RAM start (the test harness's level select for SHK,
7:	btst	#3, 0xC00005                    |  written at a frame's start once the title set game mode $8C)
	beq.s	7b                              |  lands before the choice; the VBlank stays pending and the
	bsr.s	zone                            |  switch's stub absorbs it, so the count is the same
	bra.s	switch
zone:	moveq	#0, %d0
	move.b	0xFFFFFE10, %d0
	cmpi.b	#0x17, %d0
	bls.s	2f
	moveq	#0, %d0
2:	add.w	%d0, %d0
	tst.b	0xFFFFFE11
	beq.s	3f
	addq.w	#1, %d0
3:	lea	acts(%pc), %a0
	move.b	(%a0, %d0.w), %d7
	rts
switch:
	move.w	#0x2700, %sr
	lea	0xFFFE00, %sp                   | Level is reached with a deep stack: it would run over the
	                                        | copied stub - nothing on it has to survive the switch
	lea	stub(%pc), %a0
	lea	0xFFFD20, %a1
	move.w	#(stubend - stub) / 2 - 1, %d0
4:	move.w	(%a0)+, (%a1)+
	dbra	%d0, 4b
	jmp	0xFFFD20
	.include "tables.inc"
	.even
stub:	.incbin	"swap.bin"
	.even
stubend:
