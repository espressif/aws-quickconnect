	.bss

	.global tsens_out
tsens_out:	.long 0

	.text
	.global entry
entry:

	move r1, tsens_out    // Get adress of tsens_out
	tsens r0, 1000
	st r0, r1, 0  // Save the  in memory
	jump entry