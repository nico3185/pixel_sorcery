| PixelSorcery Mega CD game-select menu: runs from word RAM ($200000), loaded by the sub CPU.
| Shows the picture (menu_art.py: CRAM, name table, tiles - incbin'd), draws a yellow frame of
| sprites around the selected card, reads pad 1 (3-button) and, on Start/A over a game the disc
| holds, calls the IP's launcher (a5) with the game number in d0 (1..8, cards left to right, top
| row first). It does not return.
| Assembled with --defsym YPAL=p,YIDX=i: the palette and colour index of the art's yellow.
	.text
	.globl	_start
	.equ	VCTRL, 0xC00004
	.equ	VDATA, 0xC00000
	.equ	SPRTILE, 1100                    | frame tiles: top, bottom, left, right
_start:
	move.w	#0x2700, %sr
	movea.l	#0xFFFE00, %sp
	lea	VCTRL, %a0
	lea	VDATA, %a1
	lea	vdpregs(%pc), %a2
1:	move.w	(%a2)+, %d0
	beq.s	2f
	move.w	%d0, (%a0)
	bra.s	1b
2:	move.l	#0x40000000, (%a0)              | clear VRAM
	move.w	#0x7FFF, %d1
3:	move.w	#0, (%a1)
	dbra	%d1, 3b
	move.l	#0x40000010, (%a0)              | VSRAM: no vertical scroll (the BIOS leaves its own)
	move.l	#0, (%a1)
	lea	art(%pc), %a2
	move.l	#0xC0000000, (%a0)              | CRAM: 64 colours
	moveq	#63, %d1
4:	move.w	(%a2)+, (%a1)
	dbra	%d1, 4b
	lea	2240(%a2), %a3                  | tiles follow the 40x28 name table
	move.l	#0x40200000, (%a0)              | tiles from VRAM $0020 (tile 1)
	move.w	#TILEWORDS - 1, %d1
5:	move.w	(%a3)+, (%a1)
	dbra	%d1, 5b
	moveq	#0, %d2                          | name table rows -> plane A ($C000, 64 cells wide)
6:	move.l	%d2, %d0
	lsl.l	#7, %d0                          | row * 128
	add.l	#0xC000, %d0
	bsr	vramw
	moveq	#39, %d1
7:	move.w	(%a2)+, (%a1)
	dbra	%d1, 7b
	addq.l	#1, %d2
	cmp.l	#28, %d2
	bne.s	6b
	move.l	#SPRTILE * 32, %d0              | the frame's four tiles, in the art's yellow
	bsr	vramw
	move.l	#YIDX * 0x11111111, %d3
	move.l	%d3, (%a1)                      | top: rows 0-1
	move.l	%d3, (%a1)
	moveq	#5, %d1
8:	move.l	#0, (%a1)
	dbra	%d1, 8b
	moveq	#5, %d1                          | bottom: rows 6-7
9:	move.l	#0, (%a1)
	dbra	%d1, 9b
	move.l	%d3, (%a1)
	move.l	%d3, (%a1)
	move.l	#YIDX * 0x11000000, %d4          | left: columns 0-1
	moveq	#7, %d1
10:	move.l	%d4, (%a1)
	dbra	%d1, 10b
	move.l	#YIDX * 0x00000011, %d4          | right: columns 6-7
	moveq	#7, %d1
11:	move.l	%d4, (%a1)
	dbra	%d1, 11b
	move.b	#0x40, 0xA10009                 | pad 1: TH as output
	move.w	#0x8164, (%a0)                  | display on
	moveq	#0, %d6                          | selection
	moveq	#-1, %d7                         | previous buttons (all held: no edge at start)
loop:
	bsr	vblank
	bsr	frame
	bsr	pad                              | d0: buttons, 1 = pressed (UDLRBCAS)
	move.w	%d0, %d1
	not.w	%d7
	and.w	%d7, %d1                         | newly pressed
	move.w	%d0, %d7
	btst	#2, %d1                          | left
	beq.s	12f
	move.w	%d6, %d0
	and.w	#3, %d0
	beq.s	12f
	subq.w	#1, %d6
12:	btst	#3, %d1                          | right
	beq.s	13f
	move.w	%d6, %d0
	and.w	#3, %d0
	cmp.w	#3, %d0
	beq.s	13f
	addq.w	#1, %d6
13:	btst	#0, %d1                          | up
	beq.s	14f
	cmp.w	#4, %d6
	blt.s	14f
	subq.w	#4, %d6
14:	btst	#1, %d1                          | down
	beq.s	15f
	cmp.w	#4, %d6
	bge.s	15f
	addq.w	#4, %d6
15:	and.w	#0xC0, %d1                       | A or Start
	beq.s	loop
	lea	avail(%pc), %a2
	tst.b	(%a2, %d6.w)
	beq.s	loop                              | not on this disc
	moveq	#0, %d0
	move.w	%d6, %d0
	addq.w	#1, %d0
	jmp	(%a5)                            | the IP's launcher

| VRAM write command for the address in d0
vramw:	move.l	%d0, %d5
	and.l	#0x3FFF, %d5
	swap	%d5
	lsr.l	#8, %d0
	lsr.l	#6, %d0
	and.l	#3, %d0
	or.l	%d5, %d0
	or.l	#0x40000000, %d0
	move.l	%d0, (%a0)
	rts

vblank:	move.w	(%a0), %d0                       | wait for the start of VBlank
	btst	#3, %d0
	bne.s	vblank
1:	move.w	(%a0), %d0
	btst	#3, %d0
	beq.s	1b
	rts

| 3-button pad 1 -> d0 bits: 0 up 1 down 2 left 3 right 4 B 5 C 6 A 7 Start (1 = pressed)
pad:	move.b	#0x40, 0xA10003
	nop
	nop
	move.b	0xA10003, %d0                   | ?1CBRLDU
	move.b	#0x00, 0xA10003
	nop
	nop
	move.b	0xA10003, %d1                   | ?0SA00DU
	and.w	#0x3F, %d0
	and.w	#0x30, %d1
	lsl.w	#2, %d1                          | A -> bit 6, Start -> bit 7
	or.w	%d1, %d0
	not.w	%d0
	and.w	#0xFF, %d0
	rts

| the frame around card d6: 36 sprites (9 x top, bottom, left, right edges), 72 x 72 px
frame:	move.l	#0x78000003, (%a0)              | sprite table $F800
	lea	cards(%pc), %a2
	move.w	%d6, %d0
	lsl.w	#2, %d0
	movem.w	(%a2, %d0.w), %d2-%d3            | x, y of the frame's top-left (sprite coordinates)
	moveq	#1, %d4                          | sprite link
	moveq	#8, %d1
1:	move.w	%d3, (%a1)                      | top edge
	move.w	%d4, (%a1)
	move.w	#(YPAL << 13) + 0x8000 + SPRTILE, (%a1)
	move.w	%d2, %d5
	moveq	#8, %d0
	sub.w	%d1, %d0
	lsl.w	#3, %d0
	add.w	%d0, %d5
	move.w	%d5, (%a1)
	addq.w	#1, %d4
	dbra	%d1, 1b
	moveq	#8, %d1
2:	move.w	%d3, %d5                        | bottom edge (64 px below)
	add.w	#64, %d5
	move.w	%d5, (%a1)
	move.w	%d4, (%a1)
	move.w	#(YPAL << 13) + 0x8000 + SPRTILE + 1, (%a1)
	move.w	%d2, %d5
	moveq	#8, %d0
	sub.w	%d1, %d0
	lsl.w	#3, %d0
	add.w	%d0, %d5
	move.w	%d5, (%a1)
	addq.w	#1, %d4
	dbra	%d1, 2b
	moveq	#8, %d1
3:	move.w	%d3, %d5                        | left edge
	moveq	#8, %d0
	sub.w	%d1, %d0
	lsl.w	#3, %d0
	add.w	%d0, %d5
	move.w	%d5, (%a1)
	move.w	%d4, (%a1)
	move.w	#(YPAL << 13) + 0x8000 + SPRTILE + 2, (%a1)
	move.w	%d2, (%a1)
	addq.w	#1, %d4
	dbra	%d1, 3b
	moveq	#8, %d1
4:	move.w	%d3, %d5                        | right edge (64 px right)
	moveq	#8, %d0
	sub.w	%d1, %d0
	lsl.w	#3, %d0
	add.w	%d0, %d5
	move.w	%d5, (%a1)
	cmp.w	#36, %d4
	bne.s	5f
	moveq	#0, %d4                          | last sprite: end of the list
5:	move.w	%d4, (%a1)
	move.w	#(YPAL << 13) + 0x8000 + SPRTILE + 3, (%a1)
	move.w	%d2, %d5
	add.w	#64, %d5
	move.w	%d5, (%a1)
	tst.w	%d4
	beq.s	6f
	addq.w	#1, %d4
6:	dbra	%d1, 4b
	rts

vdpregs:
	.word	0x8004, 0x8124, 0x8230, 0x8407, 0x857C, 0x8700, 0x8B00, 0x8C81
	.word	0x8D3F, 0x8F02, 0x9001, 0x9100, 0x9200, 0
| card frames, top-left in sprite coordinates (screen + 128): 4 x 2 grid
cards:	.word	13 + 128, 39 + 128, 88 + 128, 39 + 128, 163 + 128, 39 + 128, 238 + 128, 39 + 128
	.word	13 + 128, 114 + 128, 88 + 128, 114 + 128, 163 + 128, 114 + 128, 238 + 128, 114 + 128
	.ascii	"AVAIL!!!"                       | build_disc.py: 1 where the disc holds that game
avail:	.byte	0, 0, 0, 0, 0, 0, 0, 0
	.even
art:	.incbin	"menu_art.bin"
	.equ	TILEWORDS, (artend - art - 128 - 2240) / 2
artend:
