| SH1 on the Mega CD compilation, one image per zone (sh1_disc.sh): the game is cut into images
| that all keep its code and shared data at the same place in word RAM ($200000 + ROM address, ROM
| $000000-$03DCF2) and differ in the zone data held in the program-RAM window ($020000-$03FFFF).
| Two entries, at $23F000 (relocate.py --blob):
|   +0  the main loop's dispatch (MainGameLoop's move/andi/jsr patched to jsr here): the game modes
|       this image holds (MODES, bit = mode / 4) run as before; the others switch to the front image
|   +4  the Level routine's entry (its first instruction patched to jmp here): the zones this image
|       holds (ZONE, ZONE2) load as before; another zone switches to that zone's image
| A switch copies the stub (swap.s, its descriptor at DESC) below the stack and runs it with d7 = $80 +
| group (zone 0 and the ending: group 1, the card; zones 1-5: groups 13-17, Final Zone - zone 5 act 2 -
| group 18). Nothing in work RAM has
| to survive but the game's own state: the new image starts at the main loop, which reads the mode.
	.text
	.globl	_start
_start:
	bra.w	dispatch
	bra.w	level
dispatch:
	moveq	#0, %d0
	move.b	0xFFFFF600, %d0
	andi.w	#0x1C, %d0
	move.w	%d0, %d1
	lsr.w	#2, %d1
	move.b	#MODES, %d2
	btst	%d1, %d2
	beq.s	tofront
	lea	0x200396, %a0                   | GameModeArray (ROM $396)
	jmp	(%a0, %d0.w)
tofront:
	moveq	#-0x80 + 1, %d7
	bra.s	switch
level:
	move.b	0xFFFFFE10, %d0                 | zone
	cmpi.b	#ZONE, %d0
	beq.s	3f
	cmpi.b	#ZONE2, %d0
	bne.s	4f
3:	.ifdef	ACTMIN                          | this image holds some acts of the zone only
	cmpi.b	#ACTMIN, 0xFFFFFE11
	blo.s	4f
	.endif
	.ifdef	ACTMAX
	cmpi.b	#ACTMAX, 0xFFFFFE11
	bhi.s	4f
	.endif
	bra.s	1f
4:	andi.w	#7, %d0
	lea	groups(%pc), %a0
	move.b	(%a0, %d0.w), %d7
	cmpi.b	#5, %d0                         | Scrap Brain's act 3 (Final Zone): its own image
	bne.s	switch
	cmpi.b	#2, 0xFFFFFE11
	bne.s	switch
	moveq	#-0x80 + 18, %d7
	bra.s	switch
1:	bset	#7, 0xFFFFF600                  | Level's own first instruction
	jmp	0x2037A8                        | then the rest of Level (ROM $37A8)
switch:
	move.w	#0x2700, %sr
	subq.l	#VBLFIX, 0xFFFFFE0C              | v_vbla_count: a VBlank falls during the load, stays pending
	                                        |  and fires as the new image starts - not a frame of the game's
	lea	0xFFFE00, %sp                   | Level is reached with a deep stack: it would run over the
	                                        | copied stub - nothing on it has to survive the switch
	lea	stub(%pc), %a0
	lea	0xFFFD20, %a1
	move.w	#(stubend - stub) / 2 - 1, %d0
2:	move.w	(%a0)+, (%a1)+
	dbra	%d0, 2b
	jmp	0xFFFD20
groups:	.byte	0x81, 0x8D, 0x8E, 0x8F, 0x90, 0x91, 0x81, 0x81
	.even
stub:	.incbin	"swap.bin"
	.even
stubend:
