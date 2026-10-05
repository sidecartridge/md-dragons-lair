@ File: picture16_asm.S
@
@ Hand-written Thumb: the inner loops of picture16's Lanczos scaler and of
@ its mixing dither, the same arithmetic as their C versions in picture16.c
@ (the host's), so the RP and a PC convert to the same bytes.
@
@ void p16_down(const uint8_t *src, uint8_t *dst, uint8_t *end,
@               const int16_t *w);
@   An output line down: out[x] = clamp((sum of w[k] * src[16 x + k],
@   k < 8) + 8192 >> 14), for dst <= &out[x] < end. src is a column of the
@   scaler's ring (16 lines a column), at the output's first tap.
@
@ void p16_across(const uint8_t *src, uint8_t *dst, uint8_t *end,
@                 const int16_t *w);
@   One phase of a line across: the outputs of a line that share their 7
@   weights, every P16_X_PERIOD-th one, each P16_X_SRC_STEP pixels further
@   on; written down a ring column, so P16_X_DST_STEP bytes apart.
@
@ Why asm: GCC's Thumb-1 code keeps the weights on the stack and shuffles
@ every pixel through a high register, about 11 cycles a tap. Here the
@ weights stay in registers (3 low, 5 high; a high one costs a mov) and the
@ taps sit at fixed offsets: 4 or 5 cycles a tap, 48 cycles an output down,
@ 43 across. The rounding is ((sum >> 13) + 1) >> 1, which equals
@ (sum + 8192) >> 14; the clamp is one unsigned compare (rarely taken).
@
@   r0 = src, r1 = sum, r2 = pixel, r3 = high weight, r4 = dst,
@   r5..r7 = w[0..2], r8..r12 = w[3..7], lr = end.

        .cpu cortex-m0plus
        .thumb
        .syntax unified

        .equ P16_RING_LINES, 16      @ PICTURE16_RING_LINES
        .equ P16_X_SRC_STEP, 11      @ SCALER_X_PERIOD_SRC
        .equ P16_X_DST_STEP, 160     @ SCALER_X_PERIOD x P16_RING_LINES

        .macro TAP_LO off, wreg
        ldrb    r2, [r0, #\off]
        muls    r2, \wreg, r2
        adds    r1, r1, r2
        .endm

        .macro TAP_HI off, wreg
        ldrb    r2, [r0, #\off]
        mov     r3, \wreg
        muls    r2, r3, r2
        adds    r1, r1, r2
        .endm

        .macro WEIGHT index, reg
        movs    r1, #(\index * 2)
        ldrsh   r2, [r3, r1]
        mov     \reg, r2
        .endm

        @ name, taps (7 or 8), source step, destination step
        .macro FIR name, taps, src_step, dst_step
        .section .time_critical.\name, "ax", %progbits
        .thumb_func
        .global \name
        .type \name, %function
\name:
        push    {r4-r7, lr}
        mov     r4, r8
        mov     r5, r9
        mov     r6, r10
        mov     r7, r11
        push    {r4-r7}
        mov     r4, r1
        mov     lr, r2
        .if \taps == 8
        WEIGHT  7, r12
        .endif
        WEIGHT  6, r11
        WEIGHT  5, r10
        WEIGHT  4, r9
        WEIGHT  3, r8
        movs    r1, #4
        ldrsh   r7, [r3, r1]
        movs    r1, #2
        ldrsh   r6, [r3, r1]
        movs    r1, #0
        ldrsh   r5, [r3, r1]
1:
        ldrb    r1, [r0, #0]
        muls    r1, r5, r1
        TAP_LO  1, r6
        TAP_LO  2, r7
        TAP_HI  3, r8
        TAP_HI  4, r9
        TAP_HI  5, r10
        TAP_HI  6, r11
        .if \taps == 8
        TAP_HI  7, r12
        .endif
        asrs    r1, r1, #13
        adds    r1, r1, #1
        asrs    r1, r1, #1
        cmp     r1, #255
        bhi     3f
2:
        strb    r1, [r4]
        adds    r4, r4, #\dst_step
        adds    r0, r0, #\src_step
        cmp     r4, lr
        bne     1b
        pop     {r4-r7}
        mov     r8, r4
        mov     r9, r5
        mov     r10, r6
        mov     r11, r7
        pop     {r4-r7, pc}
3:
        asrs    r1, r1, #31             @ below 0: 0; above 255: 255
        mvns    r1, r1
        b       2b
        .size \name, . - \name
        .endm

        FIR     p16_down, 8, P16_RING_LINES, 1
        FIR     p16_across, 7, P16_X_SRC_STEP, P16_X_DST_STEP

@ void p16_mix_rows(const mix_rows_t *a);
@   The mixing dither (mix_pixel() in picture16.c, the same arithmetic) over
@   both luma rows of a run of chroma samples: per sample, its 2 x 2 pixels
@   from its terms (mix_terms_t: red, green and blue in 8.8 with the luma's
@   offset folded in, then the four thresholds). A pixel's colour is
@   clamp((298 x luma + term) >> 8) a gun; its grid colour's choice in the
@   table is a single entry (MIX_SINGLE + entry) or a pair (mix_pair_t, 32
@   bytes); for a pair, (colour . ab - k) x recip >> 16 against the
@   threshold picks the first or the second entry. A grid colour the table
@   has not met yet (MIX_UNKNOWN) is chosen by a->choose(a->mix, colour), in
@   C, and kept in the table.
@
@ Why asm: GCC's code for the pixel runs about 117 cycles, the clamps from a
@ table in flash; here a pair's pixel is about 63, a single's about 45.
@
@   r0 = red, r1 = green, r2 = luma then blue, r3, r4 = scratch,
@   r5 = the pixel (first row), r6 = the table, r7 = the terms,
@   r8 = the pairs, r9 = 298, r10 = width, r11 = -width, lr = the end;
@   [sp] = a.

        .equ MIX_SINGLE, 0x80
        .equ MIX_UNKNOWN, 0xFF

        @ A pixel: its luma at [r5 + yoff], its threshold at [r7 + thoff].
        .macro MIX_MAIN id, yoff, thoff
        ldrb    r2, [r5, #\yoff]
        mov     r3, r9
        muls    r2, r3, r2              @ 298 x luma
        ldr     r0, [r7, #0]
        adds    r0, r0, r2
        asrs    r0, r0, #8              @ red
        cmp     r0, #255
        bhi     .Lred\id
.Lred_ok\id:
        ldr     r1, [r7, #4]
        adds    r1, r1, r2
        asrs    r1, r1, #8              @ green
        cmp     r1, #255
        bhi     .Lgreen\id
.Lgreen_ok\id:
        ldr     r3, [r7, #8]
        adds    r2, r2, r3
        asrs    r2, r2, #8              @ blue
        cmp     r2, #255
        bhi     .Lblue\id
.Lblue_ok\id:
        lsrs    r3, r0, #4
        lsls    r3, r3, #8
        lsrs    r4, r1, #4
        lsls    r4, r4, #4
        orrs    r3, r4
        lsrs    r4, r2, #4
        orrs    r3, r4                  @ the grid colour
        ldrb    r4, [r6, r3]            @ its choice
        cmp     r4, #MIX_SINGLE
        bhs     .Lnot_pair\id
.Lpair\id:
        lsls    r4, r4, #5
        add     r4, r8                  @ the pair
        ldr     r3, [r4, #0]
        muls    r3, r0, r3
        ldr     r0, [r4, #4]
        muls    r0, r1, r0
        adds    r3, r3, r0
        ldr     r0, [r4, #8]
        muls    r0, r2, r0
        adds    r3, r3, r0
        ldr     r0, [r4, #12]
        subs    r3, r3, r0              @ (colour - first) . ab
        ldr     r0, [r4, #16]
        muls    r3, r0, r3
        asrs    r3, r3, #16             @ sixty-fourths of the second entry
        ldrb    r0, [r7, #\thoff]
        subs    r0, r0, r3
        lsrs    r0, r0, #31             @ 1: threshold < proportion
        adds    r4, r4, #20
        ldrb    r0, [r4, r0]            @ the first entry or the second
.Lstore\id:
        strb    r0, [r5, #\yoff]
        .endm

        @ The pixel's paths off the main one.
        .macro MIX_STUBS id
.Lred\id:
        asrs    r0, r0, #31             @ below 0: 0; above 255: 255
        mvns    r0, r0
        lsrs    r0, r0, #24
        b       .Lred_ok\id
.Lgreen\id:
        asrs    r1, r1, #31
        mvns    r1, r1
        lsrs    r1, r1, #24
        b       .Lgreen_ok\id
.Lblue\id:
        asrs    r2, r2, #31
        mvns    r2, r2
        lsrs    r2, r2, #24
        b       .Lblue_ok\id
.Lnot_pair\id:
        cmp     r4, #MIX_UNKNOWN
        beq     .Lunknown\id
        movs    r0, #15
        ands    r0, r4                  @ a single entry
        b       .Lstore\id
.Lunknown\id:
        push    {r0-r3}
        mov     r0, lr
        push    {r0, r1}                @ the end, and 8-byte alignment
        ldr     r0, [sp, #24]           @ a
        ldr     r2, [r0, #28]           @ a->choose
        ldr     r0, [r0, #24]           @ a->mix
        movs    r1, r3                  @ the grid colour
        blx     r2
        movs    r4, r0
        pop     {r0, r1}
        mov     lr, r0
        pop     {r0-r3}
        strb    r4, [r6, r3]            @ kept for the next pixels
        cmp     r4, #MIX_SINGLE
        blo     .Lpair\id
        movs    r0, #15
        ands    r0, r4
        b       .Lstore\id
        .endm

        .section .time_critical.p16_mix_rows, "ax", %progbits
        .thumb_func
        .global p16_mix_rows
        .type p16_mix_rows, %function
p16_mix_rows:
        push    {r4-r7, lr}
        mov     r4, r8
        mov     r5, r9
        mov     r6, r10
        mov     r7, r11
        push    {r4-r7}
        push    {r0}                    @ a, at [sp]
        ldr     r5, [r0, #0]            @ a->y
        ldr     r1, [r0, #4]
        mov     lr, r1                  @ a->end
        ldr     r7, [r0, #8]            @ a->terms
        ldr     r6, [r0, #12]           @ a->table
        ldr     r1, [r0, #16]
        mov     r8, r1                  @ a->pairs
        ldr     r1, [r0, #20]
        mov     r10, r1                 @ a->width
        rsbs    r1, r1, #0
        mov     r11, r1
        movs    r1, #149
        lsls    r1, r1, #1
        mov     r9, r1                  @ 298
        b       .Lmix_loop
        MIX_STUBS 0                     @ in reach of pixel 0
.Lmix_loop:
        MIX_MAIN 0, 0, 12               @ first row
        MIX_MAIN 1, 1, 13
        b       1f
        MIX_STUBS 1
1:
        add     r5, r10                 @ second row
        MIX_MAIN 2, 0, 14
        b       2f
        MIX_STUBS 2
2:
        MIX_MAIN 3, 1, 15
        add     r5, r11
        adds    r5, r5, #2
        adds    r7, r7, #16
        cmp     r5, lr
        beq     .Lmix_end
        b       .Lmix_loop              @ out of a conditional branch's reach
        MIX_STUBS 3                     @ in reach of pixel 3
.Lmix_end:
        add     sp, #4
        pop     {r4-r7}
        mov     r8, r4
        mov     r9, r5
        mov     r10, r6
        mov     r11, r7
        pop     {r4-r7, pc}
        .size p16_mix_rows, . - p16_mix_rows
