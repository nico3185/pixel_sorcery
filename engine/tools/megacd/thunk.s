| Image switch thunk, carried by every image of a split game (relocate.py --blob at $200510,
| after the image's descriptor at $200500): the game-mode table sends the modes this image does
| not hold here. It copies the switch stub (swap.s, assembled position-independent) into the
| stack area below the main loop's return address - called from the main loop, nothing else is
| on the stack, and the stub sets the new image's stack itself - then runs it with d7 = $80 +
| GROUP (--defsym), the load request. Nothing has to survive in work RAM between switches.
	.text
	.globl	_start
_start:
	move.w	#0x2700, %sr
	moveq	#-0x80 + GROUP, %d7
	lea	stub(%pc), %a0
	lea	0xFFFD20, %a1
	move.w	#(stubend - stub) / 2 - 1, %d0
1:	move.w	(%a0)+, (%a1)+
	dbra	%d0, 1b
	jmp	0xFFFD20
	.even
stub:	.incbin	"swap.bin"
	.even
stubend:
