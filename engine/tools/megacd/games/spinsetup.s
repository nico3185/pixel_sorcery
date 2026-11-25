| The compilation disc's load screen, set up by a zone image just before it switches: sh2_zone.s selects
| program-RAM bank 2 (Sonic's art: the same in every image) and calls this at $034400 (virtual $0B4400, after the
| art), then selects bank 1 again. The 16 roll frames spinner.py --pulse drew (4 turns at 16, 24, 32, 24 pixels)
| follow at $034500; swap.s's SPIN loop turns the sprite while the disc loads, games/spinrest.s puts it away.
	.text
	.globl	_start
| the load screen, set up while this image is still in reach (swap.s turns it): both planes blank - the next
| level redraws them -, Sonic's roll frames (spinner.py, attached at $03F800, the end of program RAM's window)
| over his own art slot at VRAM $F000, sprite 0 at $F800 a 32x32 ball bottom-right (the only one: no link),
| his colours into line 0 from the target palette (the game has faded to black; it writes CRAM again itself)
_start:	lea	0xC00004, %a4
	lea	0xC00000, %a5
	move.w	#0x8F02, (%a4)                  | auto-increment 2 (the game's own)
	moveq	#0, %d0
	move.l	#0x40000003, (%a4)              | VRAM $C000: plane A
	move.w	#0x7FF, %d1
1:	move.w	%d0, (%a5)
	dbra	%d1, 1b
	move.l	#0x60000003, (%a4)              | VRAM $E000: plane B
	move.w	#0x7FF, %d1
2:	move.w	%d0, (%a5)
	dbra	%d1, 2b
	move.l	#0x60000002, (%a4)              | VRAM $A000 (the window plane's, unseen): the 16 frames
	lea	0x034500, %a0
	move.w	#0xFFF, %d1
3:	move.w	(%a0)+, (%a5)
	dbra	%d1, 3b
	move.l	#0x78000003, (%a4)              | VRAM $F800: sprite 0
	move.w	#128 + 224 - 40, (%a5)
	move.w	#0x0F00, (%a5)                  | 4x4 tiles, no link
	move.w	#0x8500, (%a5)                  | priority, line 0, tile $500 (VRAM $A000)
	move.w	#128 + 320 - 40, (%a5)
	move.l	#0xC0000000, (%a4)              | CRAM: all black first (the backdrop too: the switch comes before
	moveq	#63, %d1                        |  the game's own fade-out)
6:	move.w	%d0, (%a5)
	dbra	%d1, 6b
	move.l	#0xC0000000, (%a4)              | CRAM line 0: Sonic's colours
	lea	0xFFFFFB80, %a0
	moveq	#15, %d1
4:	move.w	(%a0)+, (%a5)
	dbra	%d1, 4b
	rts
