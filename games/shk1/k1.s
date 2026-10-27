| k1.s - SHK1: our own 68000 code (GNU as, m68k-elf), built by tools/build_shim.sh.
| Sections named .pXXXXXX are written over SH1 REV00 at $XXXXXX (make_image.py checks the
| ROM's SHA-1 first); .k1 is the shim at $80000. K_<SHK2 address> symbols (k1_reloc.inc) are
| Knuckles' code from the player's SHK2 ROM, relocated by make_image.py.
| SH1 addresses used here: Sonic_JumpHeight $13498, Sonic_JumpDirection $13248,
| MdJump $12E3C (air, not rolling), Sonic_ResetOnFloor $137A0, FindFloor $149CE,
| FindWall $14B0C, collision result tail $14E0A, Touch_Enemy $1AF30.

	.include "k1_reloc.inc"

	.equ	K_MAP,		0x86000		| Knuckles' mappings, converted to SH1's format
	.equ	K_DPLCTAB,	0x8C000		| Knuckles' DPLC (SHK format: word count, words)
	.equ	K_ART,		0xC0000		| Knuckles' art on SH1's own colours (default)
	.equ	K_ART2,		0xE0000		| ... on his colours (option KNUCKLES COLOURS)
	.equ	K_PALTAB,	0x95800		| SH1's slots 2-5 -> his (make_image.py)
	.equ	KCOL_ROM,	0x95FFE		| ROM word: 0, or 1 while the menu's ROM toggle is on
	.equ	KCOL_ART,	0xFFD5		| art set last loaded (0 / $FF)
	.equ	DMALIST,	0xC800		| (word address) SH1's art buffer, unused now

| ---------------------------------------------------------------- SH1 patches

	.section .p012c0e,"ax"		| Obj01 init: mappings
	.long	K_MAP
	.section .p01b9ba,"ax"		| special stage player: mappings
	.long	K_MAP
	.section .p01d0fa,"ax"		| ending/credits player: mappings
	.long	K_MAP
	.section .p01d132,"ax"
	.long	K_MAP

	.section .p0139c4,"ax"		| Sonic_Animate -> Knuckles' animation routine
	jmp	K_316F9A
	.section .p013c3e,"ax"		| LoadSonicDynPLC -> ours (DMA from ROM)
	jmp	K_DPLC

	.section .p000d20,"ax"		| the 4 VBlank copies of the player art DMA
	jsr	K_VBDMA
	bra.s	.+0x2A
	.section .p000e34,"ax"
	jsr	K_VBDMA
	bra.s	.+0x2A
	.section .p000f24,"ax"
	jsr	K_VBDMA
	bra.s	.+0x2A
	.section .p001030,"ax"
	jsr	K_VBDMA
	bra.s	.+0x2A

	.section .p012e3c,"ax"		| MdJump (in the air, not rolling): glide states
	jmp	K_MdAir
	.section .p0134ae,"ax"		| JumpHeight, jump released early or not: glide start
	jmp	K_JH
	.section .p01340e,"ax"		| Knuckles jumps lower: $600 ($300 under water)
	.word	0x0600
	.section .p01341a,"ax"
	.word	0x0300
	.section .p0137a0,"ax"		| Sonic_ResetOnFloor
	jmp	K_Reset
	.section .p01adfd,"ax"		| ReactToItem: Knuckles' ducking frame
	.byte	0x9C
	.section .p01af36,"ax"		| Touch_Enemy: gliding and sliding attack too
	jsr	K_Attack
	.section .p01af46,"ax"		| Touch_Enemy, boss hit: a glide turns into a fall
	jsr	K_BossHit
	nop

	| title (SHK2 style): data built by make_image.py at $90000
	.equ	K_TKNUX,	0x90000		| Knuckles' title art (Nemesis)
	.equ	K_BANNER1,	0x91800		| "KNUCKLES" (Nemesis) -> tile $520
	.equ	K_BANNER2,	0x93800		| "THE ECHIDNA IN" -> tile $5AA
	.equ	K_TSONMAP,	0x94000		| TitleSonic mappings: Knuckles
	.equ	K_PSBMAP,	0x94800		| Obj0F mappings + frame 4: the banner

	.section .p00300a,"ax"		| title: TitleSonic's art
	.long	K_TKNUX
	.section .p0030da,"ax"		| title: emblem/logo plane 4 rows lower
	.long	0x44060003
	.section .p003148,"ax"		| title: after its objects are set up
	jsr	K_TitleInit
	.section .p00a63e,"ax"		| TitleSonic: start and stop 32 lines lower
	.word	0x00FE
	.section .p00a644,"ax"
	.long	K_TSONMAP
	.section .p00a67c,"ax"
	.word	0x00B6
	.section .p00a6c0,"ax"		| PRESS START / mask: 32 lines lower
	.word	0x0150
	.section .p00a6c6,"ax"
	.long	K_PSBMAP
	.section .p00a6de,"ax"		| Obj0F init: frame 4 is the banner
	jsr	K_Obj0F
	.section .p003290,"ax"		| level select loop: no title sprites over the menu (the banner)
	jsr	K_LevSelFrame
	.section .p00a6f4,"ax"		| TM: 32 lines lower
	.word	0x0118

| ---------------------------------------------------------------- shim
	.section .k1,"ax"

K_TitleInit:				| the banner object and its art, then ExecuteObjects
	move.b	#0x0F,0xD140.w
	move.b	#4,0xD15A.w
	move.l	#0x64000002,0xC00004	| VRAM $A400 (tile $520)
	lea	K_BANNER1,%a0
	jsr	0x1440			| NemDec
	move.l	#0x75400002,0xC00004	| VRAM $B540 (tile $5AA)
	lea	K_BANNER2,%a0
	jsr	0x1440
	lea	-128(%sp),%sp		| the (C)SEGA 1991 row went to plane row 29 (off screen): copy it to 27
	movea.l	%sp,%a1
	move.l	#0x0E800003,0xC00004	| VRAM read $CE80
	moveq	#63,%d0
1:	move.w	0xC00000,(%a1)+
	dbf	%d0,1b
	movea.l	%sp,%a1
	move.l	#0x4D800003,0xC00004	| VRAM write $CD80
	moveq	#63,%d0
2:	move.w	(%a1)+,0xC00000
	dbf	%d0,2b
	lea	128(%sp),%sp
	jmp	0xD338

K_LevSelFrame:
	clr.l	0xF800.w		| sprite table: one off-screen sprite, end of list
	clr.l	0xF804.w
	move.b	#4,0xF62A.w
	rts

K_Obj0F:				| Z clear unless the TM (frame 3) is to be set up
	cmpi.b	#4,0x1A(%a0)
	bne.s	1f
	move.w	#288,8(%a0)		| banner: screen x 160, y 36
	move.w	#164,0xA(%a0)		| (Z clear)
	rts
1:	cmpi.b	#3,0x1A(%a0)
	rts

K_MdAir:				| SH1 MdJump, with SHK2's glide branch
	tst.b	0x21(%a0)
	bne.s	1f
	jsr	0x13498			| JumpHeight (patched: glide start)
	jsr	0x13248			| JumpDirection
	jmp	0x12E44			| rest of MdJump
1:	jmp	K_3156A6		| glide speed, bounds, move, glide states

K_JH:					| JumpHeight while the jump button is held
	cmp.w	0x12(%a0),%d1
	bgt.s	1f
	jmp	K_31653A		| rising slower than the cut-off: may start a glide
1:	jmp	0x134B4

K_Reset:				| landing: after a glide/climb, Knuckles' own reset
	tst.b	0x21(%a0)
	beq.s	1f
	jmp	K_316CE2
1:	btst	#4,0x22(%a0)
	jmp	0x137A6

K_Attack:				| Z set: the player hurts the enemy
	cmpi.b	#2,0x1C(%a0)		| rolling
	beq.s	1f
	cmpi.b	#1,0x21(%a0)		| gliding
	beq.s	1f
	cmpi.b	#3,0x21(%a0)		| sliding after a glide
1:	rts

K_BossHit:
	neg.w	0x10(%a0)
	neg.w	0x12(%a0)
	cmpi.b	#1,0x21(%a0)
	bne.s	1f
	move.b	#2,0x21(%a0)		| gliding into a boss: fall
	move.b	#0x21,0x1C(%a0)
	bclr	#0,0x22(%a0)
	tst.w	0x10(%a0)
	bmi.s	2f
	bset	#0,0x22(%a0)
2:	move.b	#0x13,0x16(%a0)
	move.b	#9,0x17(%a0)
1:	rts

| SH2 collision entry points with a position in d2 (y) / d3 (x) that SH1 lacks; SH1's
| solidity bits are $D (top) and $E (sides/bottom), its flip bits $800 (x) and $1000 (y).
K_FloorAt:				| S2 $318FF6
	move.b	0x17(%a0),%d0
	ext.w	%d0
	add.w	%d0,%d2
	lea	0xF768.w,%a4
	movea.w	#0x10,%a3
	move.w	#0,%d6
	moveq	#0xD,%d5
	jsr	0x149CE
	move.b	#0,%d2
	jmp	0x14E0A

K_CeilAt:				| S2 $3192E6
	move.b	0x17(%a0),%d0
	ext.w	%d0
	sub.w	%d0,%d2
	eori.w	#0xF,%d2
	lea	0xF768.w,%a4
	movea.w	#-0x10,%a3
	move.w	#0x1000,%d6
	moveq	#0xE,%d5
	jsr	0x149CE
	move.b	#0x80,%d2
	jmp	0x14E0A

K_RWallAt:				| S2 $319208
	move.b	0x17(%a0),%d0
	ext.w	%d0
	add.w	%d0,%d3
	lea	0xF768.w,%a4
	movea.w	#0x10,%a3
	move.w	#0,%d6
	moveq	#0xE,%d5
	jsr	0x14B0C
	move.b	#0xC0,%d2
	jmp	0x14E0A

K_LWallAt:				| S2 $3193D2
	move.b	0x17(%a0),%d0
	ext.w	%d0
	sub.w	%d0,%d3
	eori.w	#0xF,%d3
	lea	0xF768.w,%a4
	movea.w	#-0x10,%a3
	move.w	#0x800,%d6
	moveq	#0xE,%d5
	jsr	0x14B0C
	move.b	#0x40,%d2
	jmp	0x14E0A

| Player art: the frame's DPLC entries become a list of ROM runs at $FFC800 (count-1, then
| source.l / length in words), sent to VRAM $F000 by the VBlank DMA below.
K_DPLC:
	bsr.w	K_ColSync
	move.l	#K_MAP,4(%a0)		| SH1's BuildSprites doubles the frame with add.b (frames < $80):
	tst.b	0x1A(%a0)		| frames $80-$FA (glide, climb...) use the table from its entry $80,
	bpl.s	0f			| whose offsets make_image.py writes relative to that entry
	move.l	#K_MAP+0x100,4(%a0)
0:	moveq	#0,%d0
	move.b	0x1A(%a0),%d0
	cmp.b	0xF766.w,%d0
	beq.s	9f
	move.b	%d0,0xF766.w
	lea	K_DPLCTAB,%a2
	add.w	%d0,%d0
	adda.w	(%a2,%d0.w),%a2
	move.w	(%a2)+,%d5
	subq.w	#1,%d5
	bmi.s	9f
	lea	DMALIST.w,%a3
	move.w	%d5,(%a3)+
1:	moveq	#0,%d1
	move.w	(%a2)+,%d1
	move.w	%d1,%d4
	rol.w	#4,%d4
	andi.w	#0xF,%d4
	addq.w	#1,%d4			| tiles
	andi.w	#0xFFF,%d1
	lsl.l	#5,%d1
	addi.l	#K_ART,%d1
	tst.b	KCOL_ART.w
	beq.s	2f
	addi.l	#K_ART2-K_ART,%d1
2:
	move.l	%d1,(%a3)+
	lsl.w	#4,%d4			| words
	move.w	%d4,(%a3)+
	dbf	%d5,1b
	move.b	#1,0xF767.w
9:	rts

K_ColSync:				| KNUCKLES COLOURS option: art set and palette slots 2-5
	tst.w	KCOL_ROM
	sne	%d0
	cmp.b	KCOL_ART.w,%d0
	beq.s	3f
	move.b	%d0,KCOL_ART.w
	move.b	#0xFF,0xF766.w		| reload the frame's tiles from the other art set
3:	lea	K_PALTAB,%a1
	move.w	(%a1)+,%d1
4:	lea	0xFB04.w,%a2		| palette buffer, its fade target, the water palette
	bsr.s	5f
	lea	0xFB84.w,%a2
	bsr.s	5f
	lea	0xFA84.w,%a2
	bsr.s	5f
	lea	16(%a1),%a1
	dbf	%d1,4b
	rts
5:	movea.l	%a1,%a3			| on: SH1's 4 colours there -> his; off: his -> SH1's
	tst.b	KCOL_ART.w
	beq.s	6f
	cmpm.l	(%a3)+,(%a2)+
	bne.s	7f
	cmpm.l	(%a3)+,(%a2)+
	bne.s	7f
	move.l	(%a3)+,-8(%a2)
	move.l	(%a3)+,-4(%a2)
	rts
6:	addq.w	#8,%a3
	cmpm.l	(%a3)+,(%a2)+
	bne.s	7f
	cmpm.l	(%a3)+,(%a2)+
	bne.s	7f
	move.l	(%a1),-8(%a2)
	move.l	4(%a1),-4(%a2)
7:	rts

K_VBDMA:				| a5 = VDP control port, as in SH1's VBlank
	tst.b	0xF767.w
	beq.s	9f
	movem.l	%d0-%d4/%a1,-(%sp)
	lea	DMALIST.w,%a1
	move.w	(%a1)+,%d3		| runs - 1
	move.w	#0xF000,%d2		| VRAM destination
1:	move.l	(%a1)+,%d0
	lsr.l	#1,%d0			| source, in words
	move.w	(%a1)+,%d1		| length, in words
	move.w	%d1,%d4
	andi.w	#0xFF,%d4
	ori.w	#0x9300,%d4
	move.w	%d4,(%a5)
	move.w	%d1,%d4
	lsr.w	#8,%d4
	ori.w	#0x9400,%d4
	move.w	%d4,(%a5)
	move.w	%d0,%d4
	andi.w	#0xFF,%d4
	ori.w	#0x9500,%d4
	move.w	%d4,(%a5)
	move.w	%d0,%d4
	lsr.w	#8,%d4
	ori.w	#0x9600,%d4
	move.w	%d4,(%a5)
	swap	%d0
	andi.w	#0x7F,%d0
	ori.w	#0x9700,%d0
	move.w	%d0,(%a5)
	move.w	%d2,%d0
	andi.w	#0x3FFF,%d0
	ori.w	#0x4000,%d0
	move.w	%d0,(%a5)		| VRAM write, address bits 0-13
	move.w	%d2,%d0
	rol.w	#2,%d0
	andi.w	#3,%d0
	ori.w	#0x80,%d0		| address bits 14-15, DMA
	move.w	%d0,0xF640.w		| last command word from RAM, as SH1 does
	move.w	0xF640.w,(%a5)
	add.w	%d1,%d2
	add.w	%d1,%d2
	dbf	%d3,1b
	movem.l	(%sp)+,%d0-%d4/%a1
	move.b	#0,0xF767.w
9:	rts
