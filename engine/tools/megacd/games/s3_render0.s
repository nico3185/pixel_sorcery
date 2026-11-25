| SH3's Render_Sprites ($1942A: movea.l mappings(a0),a1 / moveq #0,d4), called here from every image (sh3_disc.sh
| patches it to jsr $200480). An object deleted after it was queued for display this frame has mappings 0: on the
| cartridge address 0 is the ROM's first bytes, zeros - a piece count of 0, nothing drawn. On the Mega CD it is the BIOS
| ROM: its words sent the next read to an odd address (address error: SH3's Gumball bonus stage froze when Tails
| was removed). Mappings 0 point at a zero word instead - what the cartridge reads.
	.text
	.globl	_start
_start:	movea.l	12(%a0), %a1
	moveq	#0, %d4
	cmpa.w	#0, %a1
	bne.s	1f
	lea	zero(%pc), %a1
1:	rts
zero:	.word	0, 0
