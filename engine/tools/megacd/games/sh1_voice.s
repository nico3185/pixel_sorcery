| SH1's SEGA voice on the Mega CD compilation. On the cartridge the Z80 plays it (27 KB of PCM, ROM
| $079688-$07FFFF) through its bank window at $078000. On the disc the front image keeps Green Hill's data where
| the voice's last 16 KB would sit in the program-RAM window (bank 1, $03C000-$03FFFF): played from there the
| voice ran into that data (noise after the first words). sh1_disc.sh puts the whole voice in program-RAM bank 2
| at the same window address ($039688); PlaySega ($71FAC: the 68000 driver, Z80 bus held, inside VBlank with the
| other interrupts masked) jumps here: bank 2 in the window, the Z80's bank at $038000, the voice started and
| waited out as on the cartridge (the same loop), then bank 1 back - nothing else reads the window meanwhile.
	.text
	move.b	#0x80, 0xA12003                 | program-RAM bank 2 at $020000-$03FFFF
	lea	0xA06000, %a0                   | the Z80's bank: 9 writes of one bit, A15 first, up to A23
	move.l	#0x038000, %d0
	lsr.l	#7, %d0                         | A15 in bit 8
	lsr.l	#8, %d0                         | A15 in bit 0
	moveq	#8, %d1
1:	move.b	%d0, (%a0)
	lsr.l	#1, %d0
	dbra	%d1, 1b
	move.b	#0x88, 0xA01FFF                 | the driver's SEGA sample, as PlaySega
	move.w	#0, 0xA11100                    | the Z80 runs
	move.w	#0x11, %d1
2:	move.w	#-1, %d0
3:	nop
	dbra	%d0, 3b
	dbra	%d1, 2b
	move.b	#0x40, 0xA12003                 | bank 1 back
	addq.w	#4, %sp                         | as PlaySega: return past the sound driver's caller
	rts
