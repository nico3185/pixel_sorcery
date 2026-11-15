| SH3 engine on the Mega CD, one image per zone (sh3_disc.sh): the sound driver is uploaded to Z80
| RAM once, at boot, from the image running then - but each image moves the driver's ROM banks into its own
| word-RAM windows (z80plan_s3.py), so the driver's bank tables (DAC $DC, music $B48, the switch sites, the
| SFX sequences) must be rewritten for every image entered. The image's descriptor enters here: take the Z80
| bus, apply this image's records (z80patch.bin: offset.w, count - 1.w, bytes, padded even; $FFFF ends),
| release it, and go on to the main loop (MAINLOOP). Balloon Park ran the front's plan: its song read bank
| $1B from window $44, which held other data, and a DAC request walked HL through RAM - the 68000 hung.
	.text
	.globl	_start
_start:
	move.w	#0x100, 0xA11100
1:	btst	#0, 0xA11100
	bne.s	1b
	lea	records(%pc), %a0
2:	move.w	(%a0)+, %d0
	bmi.s	9f
	move.w	(%a0)+, %d1
	lea	0xA00000, %a1
	adda.w	%d0, %a1
3:	move.b	(%a0)+, (%a1)+
	dbra	%d1, 3b
	move.l	%a0, %d0                        | records are padded to an even length
	addq.l	#1, %d0
	andi.b	#0xFE, %d0
	movea.l	%d0, %a0
	bra.s	2b
9:	move.w	#0, 0xA11100
	jmp	MAINLOOP
	.even
records:	.incbin	"z80patch.bin"
	.even
