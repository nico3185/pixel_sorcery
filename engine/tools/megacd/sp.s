| PixelSorcery Mega CD boot, sub CPU (SP): the BIOS module header and its entry table.
| Main: on each main-CPU request - flag $5A: load group 0, flag $80 + N: load group N - read that
| group's images from the disc (BIOS CD calls) to their destinations (word RAM: $080000 here,
| $200000 for the main CPU; program RAM), hand word RAM back (RET) and echo the flag.
	.text
	.globl	_start
	.equ	CDBIOS, 0x5F22
	.equ	ROMREADN, 0x0020
	.equ	CDCSTAT, 0x008A
	.equ	CDCREAD, 0x008B
	.equ	CDCTRN, 0x008C
	.equ	CDCACK, 0x008D
	.equ	ROMPAUSEON, 0x0003
	.equ	GROUPSZ, 6 * 12 + 4
_start:
	.ascii	"MAIN       "              | module name (11 bytes)
	.byte	0                         | flag
	.word	0x0100                    | version
	.word	0                         | type
	.long	0                         | next module
	.long	end - _start              | module size
	.long	table - _start            | entry table offset
	.long	0                         | work RAM size
table:
	.word	init - table, main - table, irq2 - table, user - table, 0
init:	bclr	#2, 0xFF8003                    | MODE 0: word RAM as one 256 KB block (2M)
	move.b	#0x10, 0xFF800F                 | sub flag: ready
	rts
irq2:	rts
user:	rts
main:	move.b	0xFF800E, %d7                   | the BIOS leaves its own values: not a request
1:	move.b	0xFF800E, %d0                   | (no WAITVSYNC: it needs the main CPU's level-2
	cmp.b	%d7, %d0                         |  interrupt each frame) a new request?
	beq.s	1b
	moveq	#0, %d1
	cmp.b	#0x5A, %d0
	beq.s	2f
	tst.b	%d0
	bpl.s	1b
	move.b	%d0, %d1
	and.w	#0x7F, %d1                       | group N
2:	move.b	%d0, %d7
	move.b	#0x20, 0xFF800F                 | progress: loading
	mulu	#GROUPSZ, %d1
	lea	loads(%pc), %a3
	adda.w	%d1, %a3
3:	move.l	4(%a3), %d6                     | sector count; 0 ends the group
	beq.s	6f
	lea	readreq(%pc), %a0
	move.l	(%a3), (%a0)                    | start LBA
	move.l	%d6, 4(%a0)
	move.w	#ROMREADN, %d0
	jsr	CDBIOS
	movea.l	8(%a3), %a2                     | destination
	subq.l	#1, %d6
4:	move.w	#CDCSTAT, %d0                   | wait for a decoded sector
	jsr	CDBIOS
	bcs.s	4b
	move.w	#CDCREAD, %d0
	jsr	CDBIOS
	bcs.s	4b
	movea.l	%a2, %a0
	lea	header(%pc), %a1
	move.w	#CDCTRN, %d0                    | 2048 bytes to (a0)
	jsr	CDBIOS
	bcs.s	4b
	move.w	#CDCACK, %d0
	jsr	CDBIOS
	lea	2048(%a2), %a2
	dbra	%d6, 4b
	lea	12(%a3), %a3
	bra.s	3b
6:	move.w	#ROMPAUSEON, %d0                | pause the drive: left playing it reads on past
	jsr	CDBIOS                          |  the data track into the audio tracks (music)
	bset	#0, 0xFF8003                    | RET: word RAM to the main CPU
	move.b	%d7, 0xFF800F                   | echo: loaded
	cmp.b	chainon(%pc), %d7               | a Mega CD game's own boot (group N = chainon):
	bne	1b                              | its module (loaded at $40000) replaces this one
	lea	chain(%pc), %a0
	lea	0x7F000, %a1
	move.w	#(chainend - chain) / 2 - 1, %d0
7:	move.w	(%a0)+, (%a1)+
	dbra	%d0, 7b
	jmp	0x7F000

| Runs at $7F000: the game's module to $6000 (where the BIOS put ours), the BIOS's call slots
| (init, main, level 2, user at $5F28..) to its entry table, then - once the main CPU has copied
| the game's IP (main flag $77) - its init, sub flag $5C for the main CPU, and its main loop.
chain:	lea	0x40000, %a0
	lea	0x6000, %a1
	move.w	#0x7000 / 4 - 1, %d0
1:	move.l	(%a0)+, (%a1)+
	dbra	%d0, 1b
	move.l	0x6018, %d1                     | entry table offset
	lea	0x6000, %a2
	adda.l	%d1, %a2
	movea.l	%a2, %a4
	lea	0x5F28, %a3
	moveq	#3, %d2
2:	move.w	(%a2)+, %d3
	lea	(%a4, %d3.w), %a5
	move.w	#0x4EF9, (%a3)+
	move.l	%a5, (%a3)+
	dbra	%d2, 2b
3:	cmp.b	#0x77, 0xFF800E                 | the main CPU has the game's IP in place
	bne.s	3b
	jsr	0x5F28                          | the game's init (as the BIOS: before its IP runs)
	move.b	#0x5C, 0xFF800F                 | sub flag $5C: go (the status/command registers
4:	tst.b	0xFF800E                        |  are the game's own protocol: left untouched)
	bne.s	4b                               | main flag 0: the main CPU is on its way
	move.b	#0, 0xFF800F
	jmp	0x5F2E                          | the game's main
chainend:
	.even
	.ascii	"CHAINON!"
chainon:	.byte	0, 0                    | build_disc.py: $80 + N of a chained Mega CD game
	.even
readreq:	.long	0, 0
header:	.long	0, 0
	.ascii	"LOADTBL!"                      | build_disc.py fills 24 groups of up to 6 x
loads:	.fill	24 * GROUPSZ, 1, 0               | (LBA, count, destination); count 0 ends a group
end:
