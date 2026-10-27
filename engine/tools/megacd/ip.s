| PixelSorcery Mega CD boot, main CPU (IP), right after the BIOS's security block (work RAM).
| Gives word RAM (2M mode) to the sub CPU, has it load group 0 and runs it: the game-select menu
| (menu.s, at $200000, called with a5 = launch) or, on a single-game disc, game 1 directly; the
| hello payload when the disc has neither. launch (d0 = game N): word RAM back to the sub CPU,
| which loads group N; then the sub CPU is halted (program RAM bank at $020000 for the main CPU),
| the game's VBlank/HBlank/external handlers go into the BIOS's jump slots, its stack is set and
| its entry point called. It stays in work RAM while the menu's word RAM is replaced.
	.text
	.globl	_start
_start:
	move.w	#0x2700, %sr
0:	cmp.b	#0x10, 0xA1200F                 | sub flag $10: word RAM is in 2M mode
	bne.s	0b
	bsr.s	togive
	move.b	#0x5A, 0xA1200E                 | main flag $5A: load group 0
	moveq	#0x5A, %d1
	bsr.s	waitload
	move.b	mode(%pc), %d0                  | 0 hello, 1 game 1 (loaded), 2 menu
	beq.s	1f
	cmp.b	#2, %d0
	beq.s	2f
	moveq	#1, %d7
	bra	start
1:	jmp	0x200000
2:	lea	launch(%pc), %a5
	jmp	0x200000

togive:	bset	#1, 0xA12003                    | DMNA: word RAM to the sub CPU
1:	btst	#0, 0xA12003                    | RET drops to 0 once the sub CPU has it
	bne.s	1b
	rts
waitload:                                       | d1: the sub flag that means "loaded"
1:	cmp.b	0xA1200F, %d1
	bne.s	1b
2:	btst	#0, 0xA12003                    | word RAM is ours again (RET)
	beq.s	2b
	rts

launch:	move.w	#0x2700, %sr
	move.w	%d0, %d7
	bsr.s	togive
	move.b	%d7, %d1
	or.b	#0x80, %d1                       | main flag $80 + N: load group N
	move.b	%d1, 0xA1200E
	bsr.s	waitload
	lea	games(%pc), %a0                 | a Mega CD game (entry SSP 'SCD!'): its own IP
	move.w	%d7, %d0
	subq.w	#1, %d0
	mulu	#22, %d0
	cmp.l	#0x53434421, (%a0, %d0.w)
	bne.s	start
	lea	scdboot(%pc), %a0               | runs from $FFF000: it overwrites this code
	lea	0xFFF000, %a1
	move.w	#(scdend - scdboot) / 2 - 1, %d0
1:	move.w	(%a0)+, (%a1)+
	dbra	%d0, 1b
	jmp	0xFFF000
| the game's boot sector + IP are in word RAM: its code (disc $76E-$FFF) to $FF056E, where the
| BIOS puts it after the security block; then, with the sub CPU running its module, jump in
scdboot:
	lea	0x200000 + 0x76E, %a0
	lea	0xFF056E, %a1
	move.w	#(0x1000 - 0x76E) / 2 - 1, %d0
1:	move.w	(%a0)+, (%a1)+
	dbra	%d0, 1b
	move.b	#0x77, 0xA1200E                 | main flag $77: copied
2:	cmp.b	#0x5C, 0xA1200F                 | sub flag $5C: its module is initialised
	bne.s	2b
	move.b	#0, 0xA1200E                    | both flags back to 0 for the game
3:	tst.b	0xA1200F
	bne.s	3b
	movea.l	#0xFFFD00, %sp
	move.w	#0x2000, %sr                    | interrupts on, as the BIOS enters an IP: its
	jmp	0xFF056E                        | VBlank raises the sub CPU's level 2 (SHCD's module runs on it)
scdend:
start:	lea	swapstub(%pc), %a0              | image switch stub (swap.s) to $FFFD18: images
	lea	0xFFFD18, %a1                   | of a game too big for the main CPU call it
	move.w	#(swapend - swapstub) / 2 - 1, %d0
7:	move.w	(%a0)+, (%a1)+
	dbra	%d0, 7b
	move.w	#0xFD0C, 0xA12006               | HBlank vector: the main CPU's level 4 comes from
	bset	#1, 0xA12001                    | this register on a Mega CD - the BIOS's HBlank slot
1:	btst	#1, 0xA12001                    | (SBRQ above: halt the sub CPU, its work is done)
	beq.s	1b
	lea	games(%pc), %a0                 | entry N: SSP, VBlank, HBlank, ext, bank word, entry
	move.w	%d7, %d0
	subq.w	#1, %d0
	mulu	#22, %d0
	adda.w	%d0, %a0
	move.b	17(%a0), 0xA12003               | program RAM bank at $020000 (BK1-0 in bits 7-6)
	move.w	#0x4EF9, 0xFFFD06               | BIOS level 6 (VBlank) slot: JMP game handler
	move.l	4(%a0), 0xFFFD08
	move.w	#0x4EF9, 0xFFFD0C               | level 4 (HBlank)
	move.l	8(%a0), 0xFFFD0E
	move.w	#0x4EF9, 0xFFFD12               | level 2 (external)
	move.l	12(%a0), 0xFFFD14
	movea.l	(%a0), %sp                      | the game's stack
	movea.l	18(%a0), %a1
	jmp	(%a1)                           | its entry point
	.even
swapstub:
	.incbin	"swap.bin"
	.even
swapend:
	.ascii	"GAMEVEC!"                      | build_disc.py fills: mode byte, then 8 entries
mode:	.byte	0, 0
games:	.fill	8 * 22, 1, 0
