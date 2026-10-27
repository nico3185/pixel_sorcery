| Image switch, main CPU (Mega CD compilation): a cartridge game too big for the main CPU's
| 384 KB is cut into images (front end, a zone...). The IP copies this stub to $FFFD18 (the BIOS's
| exception slots - the games' RAM clears stop below $FFFD00 and their stacks stay above $FFFDA8)
| before it starts a game. An image's thunk (moveq #$80+group,d7 / jmp $FFFD18) runs it where
| the game would go somewhere the image does not hold (e.g. its game-mode table's level entry):
|   interrupts off, the Z80 stopped; the sub CPU released and given word RAM; group request
|   (main flag = d7); once echoed and word RAM is back, the sub CPU halted again, program RAM
|   bank 1 selected; the new image's descriptor at $200500 (VBlank, HBlank, entry, stack) goes
|   into the BIOS's jump slots; the Z80 resumes and the entry runs (the game's main loop: its
|   RAM - game mode, zone - says where it is).
	.text
	.globl	_start
_start:
	move.w	#0x2700, %sr
	lea	0xA11100, %a3                   | Z80 bus request
	lea	0xA12000, %a2                   | Mega CD gate array
	move.w	#0x100, (%a3)                   | stop the Z80: its banks are in word RAM
1:	btst	#0, (%a3)
	bne.s	1b
	bclr	#1, 1(%a2)                      | release the sub CPU (SBRQ off)
2:	btst	#1, 1(%a2)
	bne.s	2b
	bset	#1, 3(%a2)                      | DMNA: word RAM to the sub CPU
3:	btst	#0, 3(%a2)
	bne.s	3b
	move.b	%d7, 14(%a2)                    | main flag $80 + group: load it
4:	cmp.b	15(%a2), %d7                    | echoed once loaded
	bne.s	4b
5:	btst	#0, 3(%a2)                      | word RAM back (RET)
	beq.s	5b
	bset	#1, 1(%a2)                      | SBRQ: halt the sub CPU (program RAM is ours)
6:	btst	#1, 1(%a2)
	beq.s	6b
	move.b	#0x40, 3(%a2)                   | program RAM bank 1 at $020000
	move.w	#0xFD0C, 6(%a2)                 | HBlank vector register: the BIOS's HBlank slot
	lea	0x200500, %a0                   | descriptor: VBlank, HBlank, entry, stack
	lea	0xFFFD06, %a1
	move.w	#0x4EF9, (%a1)+
	move.l	(%a0)+, (%a1)+
	move.w	#0x4EF9, (%a1)+
	move.l	(%a0)+, (%a1)+
	movea.l	(%a0)+, %a1
	movea.l	(%a0), %sp
	move.w	#0x8004, 0xC00004               | HBlank interrupts off: a handler address the game
	move.w	#0, (%a3)                       |  keeps in RAM belongs to the old image; the new
	                                        |  image's level init sets HBlank up again
	move.w	#0x2300, %sr
	jmp	(%a1)
