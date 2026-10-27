| SH1 on Mega CD: BuildSprites ($00D700: movea.l 4(a0),a1; moveq #0,d1) is patched into a JSR
| here. An object drawn before its init (title screen, PRESS START, one frame) has mappings 0;
| the original then reads "mappings" from the ROM's vectors (a small piece count), on a Mega CD
| from the BIOS (dozens of pieces: the 80 sprites fill up and Sonic is not drawn). Here mappings
| 0 is an empty frame.
	.text
	.globl	_start
_start:	movea.l	4(%a0), %a1
	move.l	%a1, %d1
	bne.s	1f
	lea	empty(%pc), %a1
1:	moveq	#0, %d1
	rts
empty:	.rept	128
	.word	256                             | every frame: the zero piece count below
	.endr
	.byte	0
	.even
