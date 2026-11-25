| The compilation disc's load screen, put away once the new image is in (swap.s: jsr $03FFC0, the last 64 bytes of
| program RAM's bank-1 window, kept out of the images' placement): see games/spinsetup.s.
	.text
	.globl	_start
_start:
| the game's palette in RAM to black: the new level fades out first, from black now (the picture the load screen
| left behind stays unseen); it would be black at the end of that fade anyway
	lea	0xFFFFFB00, %a0
	moveq	#31, %d1
5:	clr.l	(%a0)+
	dbra	%d1, 5b
	rts
