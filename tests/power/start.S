    .machine power8
    .abiversion 2
    .section .text.start, "ax"
    .globl _start
_start:
    lis     1, 0x0200
    li      2, 0
    bl      run
1:  b       1b
