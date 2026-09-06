/* QEMU's `pseries` reads where to put this from the program header and then
   adds four megabytes to it. So the two addresses are told apart: it is loaded
   at 0x400000, which becomes 0x800000, and it is *linked* for 0x800000, which
   is where it will actually be. Without that split every address written down
   here is four megabytes short of the truth, and anything with a table in it
   reads from nowhere. */
ENTRY(_start)
SECTIONS {
  . = 0x800000;
  .text : AT(0x400000) { *(.text.start) *(.text*) }
  .rodata : { *(.rodata*) *(.rodata.*) }
  .data : { *(.data*) *(.sdata*) }
  . = ALIGN(8);
  __bss_start = .;
  .bss : { *(.bss*) *(.sbss*) *(COMMON) }
  . = ALIGN(8);
  __bss_end = .;
  __end = .;
}
