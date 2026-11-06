/* User programs live in the low half of the address space, starting at 4 MiB. */
ENTRY(_start)
SECTIONS
{
    . = 0x400000;
    .text   : { *(.text .text.*) }
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(4096);
    .data   : { *(.data .data.*) }
    .bss    : { *(.bss .bss.*) *(COMMON) }
    /DISCARD/ : { *(.eh_frame*) *(.note .note.*) *(.comment) }
}
