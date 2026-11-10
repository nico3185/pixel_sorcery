| SH1 on the Mega CD compilation: where the SEGA screen hands over to the title (GM_Sega's
| move.b #4,(v_gamemode) at ROM $2F10, patched to jmp here - once per boot), v_vbla_count ($FFFE0C) loses the
| 2 VBlanks the disc counts more before SH1's first controller read (the game-select menu leaves VBlank
| interrupts on): the count's parity drives lost rings' floor checks and the special stage, so play would
| drift from the cartridge. Then the move, and back (its rts at $2F16).
	.text
	subq.l	#2, 0xFFFFFE0C
	move.b	#4, 0xFFFFF600
	jmp	0x202F16
