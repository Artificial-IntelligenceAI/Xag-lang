// No operating system under this: QEMU's `pseries` machine loads it where a
// kernel would go and jumps straight here.
//
// Three things have to be true before compiled code will run: a stack, a
// table-of-contents pointer, and a cleared `.bss`. The firmware provides none
// of them, and nothing about the failure when they are missing points at what
// is wrong — a table read through an unset r2 faults at an address that looks
// like nothing at all.
    .machine power8
    .abiversion 2
    .section .text.start, "ax"
    .globl _start
_start:
    lis     1, 0x0200               // a stack, well clear of everything

    // The firmware hands over with almost everything switched off, and each
    // thing missing fails in a way that points nowhere near it.
    //
    //   floating point   — the decimal unit is floating point
    //   vector, vector-scalar — the compiler reaches for these on its own when
    //                     it is moving wide integers about. Missing them looks
    //                     like a jump to 0xf40, which is not a wild address at
    //                     all: it is where a machine goes when asked for a
    //                     vector instruction it has not been allowed to run.
    //   sixty-four bit   — the worst of the three, because nothing faults. The
    //                     registers are already 64 bits wide and `std` already
    //                     stores eight bytes; what is 32-bit is the *carry*.
    //                     So `8e9 + 2e9` as a 128-bit sum comes back with a 1
    //                     in its high half, every table of wide numbers built
    //                     from adds is quietly wrong above 10^9, and what shows
    //                     up at the far end is a decimal that will not round.
    mfmsr   3
    ori     3, 3, 0x2000            // floating point
    oris    3, 3, 0x0280            // vector, and vector-scalar
    li      4, 1                    // and sixty-four bit mode, which is the top bit
    rldicr  4, 4, 63, 0
    or      3, 3, 4
    mtmsrd  3

    lis     2, .TOC.@ha             // the table of contents, at a known place
    addi    2, 2, .TOC.@l

    lis     9, __bss_start@ha       // nothing starts holding rubbish
    addi    9, 9, __bss_start@l
    lis     10, __bss_end@ha
    addi    10, 10, __bss_end@l
    li      11, 0
1:  cmpd    9, 10
    bge     2f
    std     11, 0(9)
    addi    9, 9, 8
    b       1b
2:
    // ELFv2's global entry expects r12 to hold the address being entered: the
    // first thing a function does with it is work out its own table-of-contents
    // pointer. A plain `bl` leaves it holding whatever was there before.
    lis     12, run@ha
    addi    12, 12, run@l
    mtctr   12
    bctrl
3:  b       3b
