// IBM's decimal floating-point unit, as POWER has it from POWER6.
//
// This is assembly rather than inline assembly because a decimal128 lives in a
// pair of floating-point registers and no compiler will hand one out: clang
// answers "could not allocate output register" for every quad instruction. A
// pair is easy to name here and impossible to ask for there.
//
// Everything crosses in general-purpose registers, so nothing in this file
// depends on which way round the bytes are: a decimal128 arrives as its high
// and low halves and leaves through a pointer, and the caller decides what a
// half means.
//
// ELFv2: arguments in r3 upward, the answer in r3, and f0-f13 are ours to use,
// so none of these needs a stack frame.
//
// Values cross between general and floating registers through the red zone —
// the 512 bytes below the stack pointer a leaf function may use — rather than
// through `mtfprd` and `mffprd`. Those are VSX instructions, and the decimal
// unit is older than VSX: POWER6 has one and not the other. Going through
// memory costs a store and a load and works on every machine that has the unit
// at all.

// r -> f, and f -> r, without asking for VSX.
#define TOF(f, r, slot)  std r, slot(1); lfd f, slot(1)
#define TOR(r, f, slot)  stfd f, slot(1); ld r, slot(1)

    .machine power8
    .abiversion 2
    .text

// A decimal64 comes in and goes out as the 64 bits it is.
#define ONE(name, op)                                                                    \
    .globl name;                                                                         \
    .type name, @function;                                                               \
name:                                                                                    \
    TOF(0, 3, -8);                                                                        \
    TOF(2, 4, -8);                                                                        \
    op      4, 0, 2;                                                                     \
    TOR(3, 4, -8);                                                                        \
    blr;                                                                                 \
    .size name, .-name

ONE(xag_dfp64_add, dadd)
ONE(xag_dfp64_sub, dsub)
ONE(xag_dfp64_mul, dmul)
ONE(xag_dfp64_div, ddiv)

// Less than, equal to, greater than, or unordered — which is what a
// not-a-number is, and why this cannot be three answers.
    .globl xag_dfp64_compare
    .type xag_dfp64_compare, @function
xag_dfp64_compare:
    TOF(0, 3, -8)
    TOF(2, 4, -8)
    dcmpu   0, 0, 2
    mfcr    3
    rlwinm  3, 3, 4, 28, 31       // condition register field 0 into the low bits
    cmpwi   3, 8
    beq     1f
    cmpwi   3, 4
    beq     2f
    cmpwi   3, 2
    beq     3f
    li      3, -3                 // unordered
    blr
1:  li      3, -1
    blr
2:  li      3, 1
    blr
3:  li      3, 0
    blr
    .size xag_dfp64_compare, .-xag_dfp64_compare

// A whole number into the unit, and back out of it. `dcffix` and `dctfix` are
// how a coefficient crosses without either side knowing the other's encoding.
    .globl xag_dfp64_from_whole
    .type xag_dfp64_from_whole, @function
xag_dfp64_from_whole:
    TOF(0, 3, -8)
    dcffix  2, 0
    TOR(3, 2, -8)
    blr
    .size xag_dfp64_from_whole, .-xag_dfp64_from_whole

    .globl xag_dfp64_to_whole
    .type xag_dfp64_to_whole, @function
xag_dfp64_to_whole:
    TOF(0, 3, -8)
    dctfix  2, 0
    TOR(3, 2, -8)
    blr
    .size xag_dfp64_to_whole, .-xag_dfp64_to_whole

// The power of ten a value stands at: `diex` writes one in, `dxex` reads it
// back. Both speak the biased exponent the format stores.
    .globl xag_dfp64_set_power
    .type xag_dfp64_set_power, @function
xag_dfp64_set_power:              // (biased, value)
    TOF(0, 3, -8)
    TOF(2, 4, -8)
    diex    4, 0, 2
    TOR(3, 4, -8)
    blr
    .size xag_dfp64_set_power, .-xag_dfp64_set_power

    .globl xag_dfp64_power
    .type xag_dfp64_power, @function
xag_dfp64_power:
    TOF(0, 3, -8)
    dxex    2, 0
    TOR(3, 2, -8)
    blr
    .size xag_dfp64_power, .-xag_dfp64_power

// ---- decimal128, which lives in a pair
//
// The pair is (FRT, FRT+1) and the high half is the first of the two. Halves
// arrive in registers and leave through a pointer, so this file never has to
// know which way round memory is.

#define TWO(name, op)                                                                    \
    .globl name;                                                                         \
    .type name, @function;                                                               \
name:                                                                                    \
    TOF(0, 3, -8);                                                                        \
    TOF(1, 4, -8);                                                                        \
    TOF(2, 5, -8);                                                                        \
    TOF(3, 6, -8);                                                                        \
    op      4, 0, 2;                                                                     \
    TOR(8, 4, -8);                                                                        \
    TOR(9, 5, -8);                                                                        \
    std     8, 0(7);                                                                     \
    std     9, 8(7);                                                                     \
    blr;                                                                                 \
    .size name, .-name

TWO(xag_dfp128_add, daddq)
TWO(xag_dfp128_sub, dsubq)
TWO(xag_dfp128_mul, dmulq)
TWO(xag_dfp128_div, ddivq)

    .globl xag_dfp128_compare
    .type xag_dfp128_compare, @function
xag_dfp128_compare:
    TOF(0, 3, -8)
    TOF(1, 4, -8)
    TOF(2, 5, -8)
    TOF(3, 6, -8)
    dcmpuq  0, 0, 2
    mfcr    3
    rlwinm  3, 3, 4, 28, 31
    cmpwi   3, 8
    beq     1f
    cmpwi   3, 4
    beq     2f
    cmpwi   3, 2
    beq     3f
    li      3, -3
    blr
1:  li      3, -1
    blr
2:  li      3, 1
    blr
3:  li      3, 0
    blr
    .size xag_dfp128_compare, .-xag_dfp128_compare

// A coefficient of up to thirty-four digits does not fit in a whole number, so
// it crosses in two pieces: seventeen digits shifted up, and seventeen more
// added under them. `dscliq` shifts the significand rather than scaling the
// value, so the power it stands at does not move.
    .globl xag_dfp128_from_halves
    .type xag_dfp128_from_halves, @function
xag_dfp128_from_halves:           // (high17, low17, out)
    TOF(0, 3, -8)
    dcffixq 2, 0                  // high, as a decimal128
    TOF(0, 4, -8)
    dcffixq 6, 0                  // low
    dscliq  4, 2, 17              // high, seventeen digits up
    daddq   8, 4, 6
    TOR(8, 8, -8)
    TOR(9, 9, -8)
    std     8, 0(5)
    std     9, 8(5)
    blr
    .size xag_dfp128_from_halves, .-xag_dfp128_from_halves

    .globl xag_dfp128_high17
    .type xag_dfp128_high17, @function
xag_dfp128_high17:                // (hi, lo) -> the top seventeen digits
    TOF(0, 3, -8)
    TOF(1, 4, -8)
    dscriq  2, 0, 17
    dctfixq 4, 2
    // `dctfixq` reads a pair and writes a *single* register, not the low half
    // of one: the answer is a whole number and a whole number is 64 bits.
    TOR(3, 4, -8)
    blr
    .size xag_dfp128_high17, .-xag_dfp128_high17

    .globl xag_dfp128_low17
    .type xag_dfp128_low17, @function
xag_dfp128_low17:                 // (hi, lo) -> the bottom seventeen digits
    TOF(0, 3, -8)
    TOF(1, 4, -8)
    dscliq  2, 0, 17              // throw the top away off the high end
    dscriq  4, 2, 17
    dctfixq 6, 4
    TOR(3, 6, -8)
    blr
    .size xag_dfp128_low17, .-xag_dfp128_low17

    .globl xag_dfp128_set_power
    .type xag_dfp128_set_power, @function
xag_dfp128_set_power:             // (biased, hi, lo, out)
    TOF(0, 3, -8)
    TOF(2, 4, -8)
    TOF(3, 5, -8)
    diexq   4, 0, 2
    TOR(8, 4, -8)
    TOR(9, 5, -8)
    std     8, 0(6)
    std     9, 8(6)
    blr
    .size xag_dfp128_set_power, .-xag_dfp128_set_power

    .globl xag_dfp128_power
    .type xag_dfp128_power, @function
xag_dfp128_power:
    TOF(0, 3, -8)
    TOF(1, 4, -8)
    dxexq   2, 0
    // Same again: a biased exponent is one register's worth.
    TOR(3, 2, -8)
    blr
    .size xag_dfp128_power, .-xag_dfp128_power

// ---- decimal32, which the unit does no arithmetic in
//
// z/Architecture and POWER both work at decimal64 and decimal128 only, so a
// decimal32 is widened, worked out, and rounded back — which is what the
// standard says happens anyway.
    .globl xag_dfp32_widen
    .type xag_dfp32_widen, @function
xag_dfp32_widen:
    TOF(0, 3, -8)
    dctdp   2, 0
    TOR(3, 2, -8)
    blr
    .size xag_dfp32_widen, .-xag_dfp32_widen

    .globl xag_dfp32_narrow
    .type xag_dfp32_narrow, @function
xag_dfp32_narrow:
    TOF(0, 3, -8)
    drsp    2, 0
    TOR(3, 2, -8)
    blr
    .size xag_dfp32_narrow, .-xag_dfp32_narrow

    .section .note.GNU-stack, "", @progbits
