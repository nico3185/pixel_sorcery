| A+B+C+Start: back to the game-select menu (Mega CD compilation, every cartridge card). SH1, 2, 3 and SHK
| read the pads with the same routine (ReadJoypads: SH1 $11E6, SH2 $111C, SH3 $1372, SHK $10DE),
| called once a frame from VBlank; its first instruction is patched to jump here (relocate.py --blob at the
| image's free word RAM, the same address in every image of a game). The read is the game's own, unchanged; then,
| with Start, A, B and C all held on pad 1, the menu comes back:
|   the code below copies itself to work RAM (the game's RAM: nothing of the game has to survive) and runs there:
|   sound stopped (FM keys off, PSG silent, the Z80 held in reset); the sub CPU released and given word RAM, group
|   0 loaded (the menu, and the IP that build_disc.py stores in it at $23F000); the IP copied to work RAM and
|   entered at its home entry (ip.s: a5 = its launcher, the menu at $200000) - the menu as after the boot.
	.text
	.globl	_start
_start:
	lea	(0xF604).w, %a0                 | ReadJoypads, as the game's
	lea	0xA10003, %a1
	bsr.s	pad
	addq.w	#2, %a1
	bsr.s	pad
	move.b	(0xF604).w, %d0                 | pad 1 held: SACBRLDU
	andi.b	#0xF0, %d0
	cmpi.b	#0xF0, %d0                      | Start, A, C, B
	beq.s	home
	rts
pad:	move.b	#0, (%a1)
	nop
	nop
	move.b	(%a1), %d0
	lsl.b	#2, %d0
	andi.b	#0xC0, %d0
	move.b	#0x40, (%a1)
	nop
	nop
	move.b	(%a1), %d1
	andi.b	#0x3F, %d1
	or.b	%d1, %d0
	not.b	%d0
	move.b	(%a0), %d1
	eor.b	%d0, %d1
	move.b	%d0, (%a0)+
	and.b	%d0, %d1
	move.b	%d1, (%a0)+
	rts

home:	move.w	#0x2700, %sr
	lea	run(%pc), %a0
	lea	0xFFFF0000, %a1
	move.w	#(runend - run) / 2 - 1, %d0
1:	move.w	(%a0)+, (%a1)+
	dbra	%d0, 1b
	jmp	0xFFFF0000

run:	movea.l	#0xFFFE00, %sp
	lea	0xA11100, %a3                   | Z80 bus request
	lea	0xA12000, %a2                   | gate array
	move.w	#0x100, (%a3)
1:	btst	#0, (%a3)
	bne.s	1b
	lea	0xA04000, %a4                   | FM: every channel's keys off
	lea	keys(%pc), %a0
	moveq	#5, %d1
2:	move.b	#0x28, (%a4)
	nop
	nop
	move.b	(%a0)+, 1(%a4)
	nop
	nop
	dbra	%d1, 2b
	lea	0xC00011, %a4                   | PSG: four channels silent
	move.b	#0x9F, (%a4)
	move.b	#0xBF, (%a4)
	move.b	#0xDF, (%a4)
	move.b	#0xFF, (%a4)
	move.w	#0, 0xA11200                    | the Z80 held in reset (the menu has no sound)
	move.w	#0, (%a3)
	bclr	#1, 1(%a2)                      | release the sub CPU (the games run with it halted)
3:	btst	#1, 1(%a2)
	bne.s	3b
	bset	#1, 3(%a2)                      | DMNA: word RAM to the sub CPU
4:	btst	#0, 3(%a2)
	bne.s	4b
	move.b	#0x80, 14(%a2)                  | main flag $80: load group 0
5:	cmpi.b	#0x80, 15(%a2)                  | echoed once loaded
	bne.s	5b
6:	btst	#0, 3(%a2)                      | word RAM ours again (RET)
	beq.s	6b
	lea	0x23F000, %a0                   | the IP (group 0 carries it) to work RAM
	lea	0xFFFF1000, %a1
	move.w	#0x3FF, %d0
7:	move.l	(%a0)+, (%a1)+
	dbra	%d0, 7b
	jmp	0xFFFF1002                      | ip.s home: the menu, its launcher in a5
keys:	.byte	0, 1, 2, 4, 5, 6
	.even
runend:
