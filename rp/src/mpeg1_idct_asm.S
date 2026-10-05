@ File: mpeg1_idct_asm.S
@
@ Hand-written Thumb: the inverse DCT of mpeg1_video.c (the integer
@ Chen-Wang transform of its idct_row / idct_col_put / idct_col_add /
@ idct_col_half), the same integers in the same order, so the RP decodes to
@ the same bytes as the C does on a PC.
@
@ void mpeg1_idct_rows(int16_t *b, uint32_t rows);
@   The row pass, in place, on the rows of the block whose bit is set. `b`
@   (64 coefficients, row-major) is 4-byte aligned: a row is read as words.
@ void mpeg1_idct_cols_put(const int16_t *b, uint8_t *d, int stride);
@ void mpeg1_idct_cols_add(const int16_t *b, uint8_t *d, int stride);
@   The column pass: the 8 columns as pixels into `d` (rows `stride` apart),
@   stored or added to the prediction there, clamped to 0..255.
@ void mpeg1_idct_half_put(const int16_t *b, uint8_t *d, int stride);
@ void mpeg1_idct_half_add(const int16_t *b, uint8_t *d, int stride);
@   The same for a block whose rows 4..7 are zero, with their terms dropped
@   (idct_col_half: the same integers).
@
@ Why asm: GCC's code for the M0+ moves every value through the high
@ registers and keeps the constants in literal pools far apart, about
@ 3,200 cycles a block; here about 1,750. A column keeps its even half in
@ the high registers and its odd half in the low ones, and writes its 8
@ pixels down the column with one pointer.

        .cpu cortex-m0plus
        .thumb
        .syntax unified

        .equ W1, 2841
        .equ W2, 2676
        .equ W3, 2408
        .equ W5, 1609
        .equ W6, 1108
        .equ W7, 565

@ --- Rows ----------------------------------------------------------------------

        .section .time_critical.mpeg1_idct_rows, "ax", %progbits
        .thumb_func
        .global mpeg1_idct_rows
        .type mpeg1_idct_rows, %function
mpeg1_idct_rows:
        push    {r4-r7, lr}
        mov     r2, r8
        mov     r3, r9
        mov     r4, r10
        mov     r5, r11
        push    {r2-r5}
        mov     lr, r1                  @ the rows still to do
        b       .Lrow_next
.Lrow_dc:                               @ b0 alone: every term b0 x 8
        lsls    r1, r1, #19             @ (b0 x 8) << 16
        lsrs    r2, r1, #16             @ b0 x 8
        orrs    r1, r2
        str     r1, [r0, #0]
        str     r1, [r0, #4]
        str     r1, [r0, #8]
        str     r1, [r0, #12]
        b       .Lrow_skip
.Lrow_next:
        mov     r1, lr
        lsrs    r1, r1, #1              @ carry: this row
        mov     lr, r1
        bcc     .Lrow_skip
        ldr     r1, [r0, #0]            @ b0 | b1 << 16
        ldr     r2, [r0, #4]            @ b2 | b3
        ldr     r3, [r0, #8]            @ b4 | b5
        ldr     r4, [r0, #12]           @ b6 | b7
        asrs    r5, r1, #16             @ x4 = b1
        movs    r6, r2
        orrs    r6, r3
        orrs    r6, r4
        orrs    r6, r5
        beq     .Lrow_dc
        sxth    r6, r1
        mov     r8, r6                  @ b0
        sxth    r6, r3
        mov     r9, r6                  @ b4
        sxth    r6, r2
        mov     r10, r6                 @ x3 = b2
        sxth    r6, r4
        mov     r11, r6                 @ x2 = b6
        asrs    r6, r2, #16             @ x7 = b3
        asrs    r7, r3, #16             @ x6 = b5
        asrs    r4, r4, #16             @ x5 = b7
        @ The odd half: x4 r5, x5 r4, x6 r7, x7 r6.
        adds    r1, r5, r4
        ldr     r2, =W7
        muls    r1, r2, r1              @ x8 = W7 (x4 + x5)
        ldr     r2, =W1 - W7
        muls    r5, r2, r5
        adds    r5, r5, r1              @ x4
        ldr     r2, =W1 + W7
        muls    r4, r2, r4
        subs    r4, r1, r4              @ x5
        adds    r1, r7, r6
        ldr     r2, =W3
        muls    r1, r2, r1              @ x8 = W3 (x6 + x7)
        ldr     r2, =W3 - W5
        muls    r7, r2, r7
        subs    r7, r1, r7              @ x6
        ldr     r2, =W3 + W5
        muls    r6, r2, r6
        subs    r6, r1, r6              @ x7
        adds    r1, r5, r7              @ x1 = x4 + x6: o0
        subs    r5, r5, r7              @ x4 -= x6
        adds    r7, r4, r6              @ x6 = x5 + x7: o3
        subs    r4, r4, r6              @ x5 -= x7
        movs    r2, #181
        adds    r6, r5, r4
        muls    r6, r2, r6
        adds    r6, #128
        asrs    r6, r6, #8              @ x2: o1
        subs    r5, r5, r4
        muls    r5, r2, r5
        adds    r5, #128
        asrs    r5, r5, #8              @ x4: o2
        mov     r12, r7                 @ o3
        @ The even half.
        mov     r2, r10                 @ x3
        mov     r3, r11                 @ x2
        adds    r4, r2, r3
        ldr     r7, =W6
        muls    r4, r7, r4              @ x1 = W6 (x3 + x2)
        ldr     r7, =W2 + W6
        muls    r3, r7, r3
        subs    r3, r4, r3              @ x2
        ldr     r7, =W2 - W6
        muls    r2, r7, r2
        adds    r2, r4, r2              @ x3
        mov     r4, r8
        lsls    r4, r4, #11
        adds    r4, #128                @ x0 = b0 x 2048 + 128
        mov     r7, r9
        lsls    r7, r7, #11             @ x1 = b4 x 2048
        subs    r4, r4, r7              @ x0 -= x1
        lsls    r7, r7, #1
        adds    r7, r4, r7              @ x8 = x0 + x1
        adds    r7, r7, r2              @ x7 = x8 + x3: e0
        lsls    r2, r2, #1
        subs    r2, r7, r2              @ x8 -= x3: e3
        adds    r4, r4, r3              @ x3 = x0 + x2: e1
        lsls    r3, r3, #1
        subs    r3, r4, r3              @ x0 -= x2: e2
        @ e0 r7, e1 r4, e2 r3, e3 r2; o0 r1, o1 r6, o2 r5, o3 r12.
        adds    r7, r7, r1
        lsls    r1, r1, #1
        subs    r1, r7, r1
        asrs    r7, r7, #8
        strh    r7, [r0, #0]            @ (x7 + x1) >> 8
        asrs    r1, r1, #8
        strh    r1, [r0, #14]           @ (x7 - x1) >> 8
        adds    r4, r4, r6
        lsls    r6, r6, #1
        subs    r6, r4, r6
        asrs    r4, r4, #8
        strh    r4, [r0, #2]            @ (x3 + x2) >> 8
        asrs    r6, r6, #8
        strh    r6, [r0, #12]           @ (x3 - x2) >> 8
        adds    r3, r3, r5
        lsls    r5, r5, #1
        subs    r5, r3, r5
        asrs    r3, r3, #8
        strh    r3, [r0, #4]            @ (x0 + x4) >> 8
        asrs    r5, r5, #8
        strh    r5, [r0, #10]           @ (x0 - x4) >> 8
        mov     r1, r12
        adds    r2, r2, r1
        lsls    r1, r1, #1
        subs    r1, r2, r1
        asrs    r2, r2, #8
        strh    r2, [r0, #6]            @ (x8 + x6) >> 8
        asrs    r1, r1, #8
        strh    r1, [r0, #8]            @ (x8 - x6) >> 8
.Lrow_skip:
        adds    r0, #16
        mov     r1, lr
        cmp     r1, #0
        bne     .Lrow_next
        pop     {r2-r5}
        mov     r8, r2
        mov     r9, r3
        mov     r10, r4
        mov     r11, r5
        pop     {r4-r7, pc}
        .ltorg
        .size mpeg1_idct_rows, . - mpeg1_idct_rows

@ --- Columns -------------------------------------------------------------------
@
@ The column loop: [sp] = the column's coefficients, [sp, #4] = past the
@ last column's; lr = the column's first pixel, r12 = stride. A column's
@ even half ends in r8..r11 (e0..e3), its odd half in r0, r5, r1, r3
@ (o0..o3); the pixels go down from row 0: e0 + o0, e1 + o1, e2 + o2,
@ e3 + o3, e3 - o3, e2 - o2, e1 - o1, e0 - o0, each >> 14.

        @ One pixel: r2 the pixel, r6 the value; `add` adds the prediction.
        .macro PIXEL u, id, e, op, o, add
        mov     r4, \e
        \op     r6, r4, \o
        asrs    r6, r6, #14
        .if \add
        ldrb    r7, [r2]
        adds    r6, r6, r7
        .endif
        cmp     r6, #255
        bhi     .Lclamp\u\()_\id
.Lclamped\u\()_\id:
        strb    r6, [r2]
        add     r2, r12
        .endm

        .macro CLAMP u, id
.Lclamp\u\()_\id:
        asrs    r6, r6, #31             @ below 0: 0; above 255: 255
        mvns    r6, r6
        lsrs    r6, r6, #24
        b       .Lclamped\u\()_\id
        .endm

        .macro PIXELS u, add
        mov     r2, lr
        PIXEL   \u, 0, r8, adds, r0, \add
        PIXEL   \u, 1, r9, adds, r5, \add
        PIXEL   \u, 2, r10, adds, r1, \add
        PIXEL   \u, 3, r11, adds, r3, \add
        PIXEL   \u, 4, r11, subs, r3, \add
        PIXEL   \u, 5, r10, subs, r1, \add
        PIXEL   \u, 6, r9, subs, r5, \add
        PIXEL   \u, 7, r8, subs, r0, \add
        .endm

        .macro CLAMPS u
        .irp    id, 0, 1, 2, 3, 4, 5, 6, 7
        CLAMP   \u, \id
        .endr
        .endm

        .macro COLUMNS_ENTER
        push    {r4-r7, lr}
        mov     r4, r8
        mov     r5, r9
        mov     r6, r10
        mov     r7, r11
        push    {r4-r7}
        movs    r3, r0
        adds    r3, #16
        push    {r0, r3}                @ [sp]: the column; [sp, #4]: the end
        mov     lr, r1
        mov     r12, r2
        .endm

        @ The next column, or the return.
        .macro COLUMNS_NEXT loop
        ldr     r0, [sp, #0]
        adds    r0, #2
        str     r0, [sp, #0]
        mov     r4, lr
        adds    r4, #1
        mov     lr, r4
        ldr     r4, [sp, #4]
        cmp     r0, r4
        beq     1f
        b       \loop                   @ out of a conditional branch's reach
1:
        add     sp, #8
        pop     {r4-r7}
        mov     r8, r4
        mov     r9, r5
        mov     r10, r6
        mov     r11, r7
        pop     {r4-r7, pc}
        .endm

        @ The full column pass (idct_col_put / idct_col_add).
        .macro COLUMNS_FULL name, add
        .section .time_critical.\name, "ax", %progbits
        .thumb_func
        .global \name
        .type \name, %function
\name:
        COLUMNS_ENTER
        b       .Lcolumn\@
        @ A flat column (x1..x7 zero): (b0 + 32) >> 6 down it. r8: b0.
.Lflat\@:
        mov     r0, r8
        sxth    r0, r0
        adds    r0, #32
        asrs    r0, r0, #6
        mov     r2, lr
        .if \add
        .rept 8
        ldrb    r6, [r2]
        adds    r6, r6, r0
        cmp     r6, #255
        bls     2f
        asrs    r6, r6, #31
        mvns    r6, r6
        lsrs    r6, r6, #24
2:
        strb    r6, [r2]
        add     r2, r12
        .endr
        .else
        cmp     r0, #255
        bls     2f
        asrs    r0, r0, #31
        mvns    r0, r0
        lsrs    r0, r0, #24
2:
        .rept 8
        strb    r0, [r2]
        add     r2, r12
        .endr
        .endif
        COLUMNS_NEXT .Lcolumn\@
.Lcolumn\@:
        ldr     r0, [sp, #0]
        ldrh    r1, [r0, #16]           @ x4 = b8
        ldrh    r2, [r0, #48]           @ x7 = b24
        ldrh    r3, [r0, #32]           @ x3 = b16
        ldrh    r4, [r0, #0]            @ b0
        mov     r8, r4
        adds    r0, #64
        ldrh    r4, [r0, #16]           @ x6 = b40
        ldrh    r5, [r0, #48]           @ x5 = b56
        ldrh    r6, [r0, #0]            @ x1 = b32
        ldrh    r7, [r0, #32]           @ x2 = b48
        movs    r0, r1
        orrs    r0, r2
        orrs    r0, r3
        orrs    r0, r4
        orrs    r0, r5
        orrs    r0, r6
        orrs    r0, r7
        beq     .Lflat\@
        mov     r0, r8
        sxth    r0, r0
        sxth    r1, r1
        sxth    r2, r2
        sxth    r3, r3
        sxth    r4, r4
        sxth    r5, r5
        sxth    r6, r6
        sxth    r7, r7
        @ The even half: b0 r0, x1 r6, x2 r7, x3 r3 (x6, x5 parked).
        mov     r10, r4
        mov     r11, r5
        lsls    r0, r0, #8
        ldr     r4, =8192
        adds    r0, r0, r4              @ x0 = b0 x 256 + 8192
        lsls    r6, r6, #8              @ x1 = b32 x 256
        adds    r4, r0, r6              @ x8 = x0 + x1
        subs    r0, r0, r6              @ x0 -= x1
        adds    r5, r3, r7
        ldr     r6, =W6
        muls    r5, r6, r5
        adds    r5, #4                  @ x1 = W6 (x3 + x2) + 4
        ldr     r6, =W2 + W6
        muls    r7, r6, r7
        subs    r7, r5, r7
        asrs    r7, r7, #3              @ x2
        ldr     r6, =W2 - W6
        muls    r3, r6, r3
        adds    r3, r5, r3
        asrs    r3, r3, #3              @ x3
        adds    r5, r4, r3              @ x7 = x8 + x3: e0
        subs    r4, r4, r3              @ x8 -= x3: e3
        adds    r3, r0, r7              @ x3 = x0 + x2: e1
        subs    r0, r0, r7              @ x0 -= x2: e2
        mov     r8, r5
        mov     r9, r3
        mov     r6, r10                 @ x6
        mov     r7, r11                 @ x5
        mov     r10, r0
        mov     r11, r4
        @ The odd half: x4 r1, x5 r7, x6 r6, x7 r2.
        adds    r3, r1, r7
        ldr     r4, =W7
        muls    r3, r4, r3
        adds    r3, #4                  @ x8 = W7 (x4 + x5) + 4
        ldr     r4, =W1 - W7
        muls    r1, r4, r1
        adds    r1, r1, r3
        asrs    r1, r1, #3              @ x4
        ldr     r4, =W1 + W7
        muls    r7, r4, r7
        subs    r7, r3, r7
        asrs    r7, r7, #3              @ x5
        adds    r3, r6, r2
        ldr     r4, =W3
        muls    r3, r4, r3
        adds    r3, #4                  @ x8 = W3 (x6 + x7) + 4
        ldr     r4, =W3 - W5
        muls    r6, r4, r6
        subs    r6, r3, r6
        asrs    r6, r6, #3              @ x6
        ldr     r4, =W3 + W5
        muls    r2, r4, r2
        subs    r2, r3, r2
        asrs    r2, r2, #3              @ x7
        adds    r0, r1, r6              @ x1 = x4 + x6: o0
        subs    r1, r1, r6              @ x4 -= x6
        adds    r3, r7, r2              @ x6 = x5 + x7: o3
        subs    r7, r7, r2              @ x5 -= x7
        movs    r4, #181
        adds    r5, r1, r7
        muls    r5, r4, r5
        adds    r5, #128
        asrs    r5, r5, #8              @ x2: o1
        subs    r1, r1, r7
        muls    r1, r4, r1
        adds    r1, #128
        asrs    r1, r1, #8              @ x4: o2
        PIXELS  \name, \add
        COLUMNS_NEXT .Lcolumn\@
        CLAMPS  \name
        .ltorg
        .size \name, . - \name
        .endm

        @ The column pass of a block whose rows 4..7 are zero
        @ (idct_col_half).
        .macro COLUMNS_HALF name, add
        .section .time_critical.\name, "ax", %progbits
        .thumb_func
        .global \name
        .type \name, %function
\name:
        COLUMNS_ENTER
.Lcolumn\@:
        ldr     r0, [sp, #0]
        ldrh    r1, [r0, #16]           @ x4 = b8
        ldrh    r2, [r0, #48]           @ x7 = b24
        ldrh    r3, [r0, #32]           @ x3 = b16
        ldrh    r0, [r0, #0]            @ b0
        sxth    r0, r0
        sxth    r1, r1
        sxth    r2, r2
        sxth    r3, r3
        @ The even half.
        lsls    r0, r0, #8
        ldr     r4, =8192
        adds    r0, r0, r4              @ x0 = b0 x 256 + 8192
        ldr     r4, =W6
        movs    r5, r3
        muls    r5, r4, r5
        adds    r5, #4
        asrs    r5, r5, #3              @ t2
        ldr     r4, =W2
        muls    r3, r4, r3
        adds    r3, #4
        asrs    r3, r3, #3              @ t3
        adds    r4, r0, r3
        mov     r8, r4                  @ s7 = x0 + t3: e0
        subs    r4, r0, r3
        mov     r11, r4                 @ s8 = x0 - t3: e3
        adds    r4, r0, r5
        mov     r9, r4                  @ s3 = x0 + t2: e1
        subs    r4, r0, r5
        mov     r10, r4                 @ s0 = x0 - t2: e2
        @ The odd half.
        ldr     r4, =W1
        movs    r0, r1
        muls    r0, r4, r0
        adds    r0, #4
        asrs    r0, r0, #3              @ t4
        ldr     r4, =W7
        muls    r1, r4, r1
        adds    r1, #4
        asrs    r1, r1, #3              @ t5
        ldr     r4, =W3
        movs    r3, r2
        muls    r3, r4, r3
        adds    r3, #4
        asrs    r3, r3, #3              @ t6
        ldr     r4, =W5
        muls    r2, r4, r2
        movs    r4, #4
        subs    r2, r4, r2
        asrs    r2, r2, #3              @ t7 = (4 - W5 x7) >> 3
        adds    r4, r0, r3              @ s1 = t4 + t6: o0
        subs    r0, r0, r3              @ s4 = t4 - t6
        adds    r3, r1, r2              @ s6 = t5 + t7: o3
        subs    r1, r1, r2              @ s5 = t5 - t7
        movs    r2, #181
        adds    r5, r0, r1
        muls    r5, r2, r5
        adds    r5, #128
        asrs    r5, r5, #8              @ s2: o1
        subs    r0, r0, r1
        muls    r0, r2, r0
        adds    r0, #128
        asrs    r1, r0, #8              @ s4: o2
        movs    r0, r4                  @ o0
        PIXELS  \name, \add
        COLUMNS_NEXT .Lcolumn\@
        CLAMPS  \name
        .ltorg
        .size \name, . - \name
        .endm

        COLUMNS_FULL mpeg1_idct_cols_put, 0
        COLUMNS_FULL mpeg1_idct_cols_add, 1
        COLUMNS_HALF mpeg1_idct_half_put, 0
        COLUMNS_HALF mpeg1_idct_half_add, 1
