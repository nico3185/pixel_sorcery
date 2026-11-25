| PixelSorcery Mega CD game-select menu: runs from word RAM ($200000), loaded by the sub CPU.
| Shows menu_compose.py's screen (incbin'd): plane B Green Hill's background scrolled line by line (parallax),
| plane A the cards, 4 px right; writes the selected game's name in the bottom box; draws a yellow frame of
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
2:
	.ifdef	SPLASH
	bsr	splash                           | the PixelSorcery splash first (3 s at most)
	.endif
	lea	vdpregs(%pc), %a2               | the menu's VDP set-up again (the splash has its own)
17:	move.w	(%a2)+, %d0
	beq.s	18f
	move.w	%d0, (%a0)
	bra.s	17b
18:
	move.l	#0x40000000, (%a0)              | clear VRAM
	move.w	#0x7FFF, %d1
3:	move.w	#0, (%a1)
	dbra	%d1, 3b
	move.l	#0x40000010, (%a0)              | VSRAM: plane A none, plane B 8 lines down (the BIOS leaves its own)
	move.l	#8, (%a1)
	lea	art(%pc), %a2
	move.l	#0xC0000000, (%a0)              | CRAM: 64 colours
	moveq	#63, %d1
4:	move.w	(%a2)+, (%a1)
	dbra	%d1, 4b
	lea	NAMEBYTES(%a2), %a3             | tiles follow the name tables, scroll speeds and the names
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
	move.l	#0x60000003, (%a0)              | plane B names -> $E000 (64 x 32)
	move.w	#64 * 32 - 1, %d1
16:	move.w	(%a2)+, (%a1)
	dbra	%d1, 16b
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
	move.w	#0, FCOUNT
	move.w	#-1, SHOWN
loop:
	bsr	vblank
	bsr	scroll
	bsr	frame
	bsr	name
	bsr	pad                              | d0: buttons, 1 = pressed (UDLRBCAS)
	move.w	%d0, %d1
	not.w	%d7
	and.w	%d7, %d1                         | newly pressed
	move.w	%d0, %d7
	lea	art + LAYOUT + 2(%pc), %a2      | the slots (menu_compose.py): game, x, y words, then L R U D
	move.w	%d6, %d0
	mulu	#10, %d0
	lea	6(%a2, %d0.w), %a3             | this slot's neighbours
	btst	#2, %d1                          | left
	beq.s	12f
	move.b	(%a3), %d6
12:	btst	#3, %d1                          | right
	beq.s	13f
	move.b	1(%a3), %d6
13:	btst	#0, %d1                          | up
	beq.s	14f
	move.b	2(%a3), %d6
14:	btst	#1, %d1                          | down
	beq.s	15f
	move.b	3(%a3), %d6
15:	and.w	#0xC0, %d1                       | A or Start
	beq	loop
	move.w	%d6, %d0
	mulu	#10, %d0
	move.w	(%a2, %d0.w), %d0                | the slot's game (1..8)
	lea	avail - 1(%pc), %a3
	tst.b	(%a3, %d0.w)
	beq	loop                              | not on this disc
	ext.l	%d0
	move.l	%d0, -(%sp)
	bsr	loading                          | LOADING... while the game's first image comes in
	move.l	(%sp)+, %d0                      | the game number, for the launcher
	jmp	(%a5)                            | the IP's launcher

| the line scroll table ($FC00): last frame's lines in, then the next frame's computed (plane A 4 px right,
| plane B -frame x speed / 256 per line: the clouds slowest, the water faster towards the bottom)
	.equ	FCOUNT, 0xFF8000
	.equ	SHOWN, 0xFF8002
	.equ	HSBUF, 0xFF8100
scroll:	move.l	#0x7C000003, (%a0)
	lea	HSBUF, %a2
	move.w	#223, %d1
1:	move.l	(%a2)+, (%a1)
	dbra	%d1, 1b
	addq.w	#1, FCOUNT
	move.w	FCOUNT, %d2
	lea	art + SPEEDS(%pc), %a3
	lea	HSBUF, %a2
	move.w	#223, %d1
2:	move.w	(%a3)+, %d0
	mulu	%d2, %d0
	lsr.l	#8, %d0
	neg.w	%d0
	move.w	#4, (%a2)+
	move.w	%d0, (%a2)+
	dbra	%d1, 2b
	rts

| the selected game's name in the bottom box (plane A row 24, columns 2-36), when the selection changes - or
| NOT ON THIS DISC for a card whose game the disc does not hold
name:	cmp.w	SHOWN, %d6
	beq.s	1f
	move.w	%d6, SHOWN
	move.l	#0xC000 + 24 * 128 + 2 * 2, %d0
	bsr	vramw
	lea	art + LAYOUT + 2(%pc), %a2      | the slot's game
	move.w	%d6, %d0
	mulu	#10, %d0
	move.w	(%a2, %d0.w), %d0
	subq.w	#1, %d0
	lea	art + GNAMES(%pc), %a2
	lea	avail(%pc), %a3
	tst.b	(%a3, %d0.w)
	bne.s	3f
	moveq	#8, %d0                          | a game the disc lacks: NOT ON THIS DISC
3:	mulu	#70, %d0
	adda.w	%d0, %a2
	moveq	#34, %d1
2:	move.w	(%a2)+, (%a1)
	dbra	%d1, 2b
1:	rts

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

| the LOADING banner (build_disc.py's loading.bin): three 32 x 24 sprites, centred, over the menu - the frame
| sprites are cut off the list (sprite 2 ends it)
	.equ	LTILE, SPRTILE + 4
	.equ	STILE, LTILE + 36                    | ip.s SPINLOAD turns these: keep in step
loading:
	bsr	vblank
	move.l	#LTILE * 32, %d0
	bsr	vramw
	lea	ldart(%pc), %a2
	move.w	#36 * 16 - 1, %d1
1:	move.w	(%a2)+, (%a1)
	dbra	%d1, 1b
	move.l	#STILE * 32, %d0                | Sonic's 16 roll frames (build_disc.py's spinld.bin)
	bsr	vramw
	lea	spinld(%pc), %a2
	move.w	#16 * 16 * 16 - 1, %d1
4:	move.w	(%a2)+, (%a1)
	dbra	%d1, 4b
	move.l	#0x78000003, (%a0)              | sprite table $F800
	moveq	#0, %d2
2:	move.w	#128 + 100, (%a1)               | y
	move.w	%d2, %d0                        | size 4 x 3, link k + 1 (the last to Sonic, sprite 3)
	addq.w	#1, %d0
	or.w	#0x0E00, %d0
	move.w	%d0, (%a1)
	move.w	%d2, %d0
	mulu	#12, %d0
	add.w	#(YPAL << 13) + 0x8000 + LTILE, %d0
	move.w	%d0, (%a1)
	move.w	%d2, %d0
	lsl.w	#5, %d0
	add.w	#128 + 112, %d0
	move.w	%d0, (%a1)
	addq.w	#1, %d2
	cmp.w	#3, %d2
	bne.s	2b
	move.w	#128 + 224 - 40, (%a1)          | sprite 3: Sonic, 32 x 32 bottom-right, the end of the list
	move.w	#0x0F00, (%a1)
	move.w	#(YPAL << 13) + 0x8000 + STILE, (%a1)
	move.w	#128 + 320 - 40, (%a1)
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

| the frame around card d6: 34 sprites (9 x top and bottom, 8 x left and right edges), 72 x 64 px
frame:	move.l	#0x78000003, (%a0)              | sprite table $F800
	lea	art + LAYOUT + 2(%pc), %a2
	move.w	%d6, %d0
	mulu	#10, %d0
	movem.w	2(%a2, %d0.w), %d2-%d3           | x, y of the frame's top-left (sprite coordinates)
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
2:	move.w	%d3, %d5                        | bottom edge (56 px below)
	add.w	#56, %d5
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
	moveq	#7, %d1
3:	move.w	%d3, %d5                        | left edge
	moveq	#7, %d0                          | 8 sprites: 0..56 px down
	sub.w	%d1, %d0
	lsl.w	#3, %d0
	add.w	%d0, %d5
	move.w	%d5, (%a1)
	move.w	%d4, (%a1)
	move.w	#(YPAL << 13) + 0x8000 + SPRTILE + 2, (%a1)
	move.w	%d2, (%a1)
	addq.w	#1, %d4
	dbra	%d1, 3b
	moveq	#7, %d1
4:	move.w	%d3, %d5                        | right edge (64 px right)
	moveq	#7, %d0                          | 8 sprites: 0..56 px down
	sub.w	%d1, %d0
	lsl.w	#3, %d0
	add.w	%d0, %d5
	move.w	%d5, (%a1)
	cmp.w	#34, %d4
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

	.ifdef	SPLASH
| splash.py's splash: the icon turns once while it grows (NF frames, a new one every 3 VBlanks: 20 fps), double
| buffered (tiles 1-256 / 257-512), centred on plane A; then 90 frames (1.5 s) with Sonic rolling bottom-right
| (sprite 0, tiles 513-768, palette line 3) under the repository's URL and the years. Leaves the display off; the menu
| sets everything up again.
	.equ	SNF, 20
	.equ	SFRAME, 512 + 8192
splash:	move.l	#0x40000000, (%a0)              | clear VRAM (plane A, sprites)
	move.w	#0x7FFF, %d1
1:	move.w	#0, (%a1)
	dbra	%d1, 1b
	move.l	#0x40000010, (%a0)
	move.l	#0, (%a1)
	lea	sdata, %a2                      | absolute: past a 16-bit displacement
	move.l	#0xC0000000, (%a0)              | CRAM: the icon's 3 lines, Sonic's
	moveq	#63, %d1
2:	move.w	(%a2)+, (%a1)
	dbra	%d1, 2b
	move.w	#0x8164, (%a0)                  | display on (black: nothing drawn yet)
	moveq	#0, %d6                          | frame
3:	move.w	%d6, %d0                        | tiles of frame d6 -> buffer d6 & 1
	and.w	#1, %d0
	mulu	#8192, %d0
	add.l	#0x20, %d0
	bsr	vramw
	lea	512(%a2), %a3
	move.w	#4095, %d1
4:	move.w	(%a3)+, (%a1)
	dbra	%d1, 4b
	bsr	vblank
	moveq	#0, %d4                          | name table: 16 rows from cell (12, 6), +256 for buffer 1
	btst	#0, %d6
	beq.s	5f
	move.w	#256, %d4
5:	movea.l	%a2, %a3
	moveq	#0, %d2
6:	move.l	%d2, %d0
	add.l	#6, %d0
	lsl.l	#7, %d0
	add.l	#0xC000 + 12 * 2, %d0
	bsr	vramw
	moveq	#15, %d1
7:	move.w	(%a3)+, %d0
	add.w	%d4, %d0
	move.w	%d0, (%a1)
	dbra	%d1, 7b
	addq.l	#1, %d2
	cmp.l	#16, %d2
	bne.s	6b
	bsr	vblank
	bsr	vblank
	adda.l	#SFRAME, %a2
	addq.w	#1, %d6
	cmp.w	#SNF, %d6
	bne.w	3b
	move.l	#0x40200001, (%a0)              | Sonic's 16 roll frames -> tile 513 (VRAM $4020)
	move.w	#4095, %d1
8:	move.w	(%a2)+, (%a1)
	dbra	%d1, 8b
	move.w	(%a2)+, %d3                      | the text: tiles -> 769 (VRAM $6020), names -> rows 22-25
	lea	320(%a2), %a3
	move.l	#0x60200001, (%a0)
	mulu	#16, %d3
	subq.w	#1, %d3
	bmi.s	10f
12:	move.w	(%a3)+, (%a1)
	dbra	%d3, 12b
10:	moveq	#0, %d2
11:	move.l	%d2, %d0
	add.l	#22, %d0
	lsl.l	#7, %d0
	add.l	#0xC000, %d0
	bsr	vramw
	moveq	#39, %d1
13:	move.w	(%a2)+, (%a1)
	dbra	%d1, 13b
	addq.l	#1, %d2
	cmp.l	#4, %d2
	bne.s	11b
	moveq	#0, %d6
9:	bsr	vblank
	move.l	#0x78000003, (%a0)              | sprite 0: 32 x 32, bottom-right, a roll frame every 4 VBlanks
	move.w	#128 + 224 - 40, (%a1)
	move.w	#0x0F00, (%a1)
	move.w	%d6, %d0
	lsr.w	#2, %d0
	and.w	#15, %d0
	lsl.w	#4, %d0
	add.w	#0xE000 + 513, %d0               | priority, line 3
	move.w	%d0, (%a1)
	move.w	#128 + 320 - 40, (%a1)
	addq.w	#1, %d6
	cmp.w	#90, %d6
	bne.s	9b
	bsr	vblank
	move.w	#0x8124, (%a0)                  | display off
	rts
	.endif

vdpregs:
	.word	0x8004, 0x8124, 0x8230, 0x8407, 0x857C, 0x8700, 0x8B03, 0x8C81
	.word	0x8D3F, 0x8F02, 0x9001, 0x9100, 0x9200, 0
	.ascii	"AVAIL!!!"                       | build_disc.py: 1 where the disc holds that game
avail:	.byte	0, 0, 0, 0, 0, 0, 0, 0
	.even
ldart:	.incbin	"loading.bin"
spinld:	.incbin	"spinld.bin"
art:	.incbin	"menu_art.bin"
	.equ	SPEEDS, 128 + 2240 + 4096            | menu_compose.py's layout after the CRAM
	.equ	GNAMES, SPEEDS + 448
	.equ	LAYOUT, GNAMES + 630
	.equ	NAMEBYTES, 2240 + 4096 + 448 + 630 + 82
	.equ	TILEWORDS, (artend - art - 128 - NAMEBYTES) / 2
artend:
	.ifdef	SPLASH
	.even
sdata:	.incbin	"splash.bin"
	.endif
