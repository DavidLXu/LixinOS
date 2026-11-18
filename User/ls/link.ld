ENTRY(_start)
PHDRS { text PT_LOAD FLAGS(5); }
SECTIONS {
    . = 0x44000000;
    .text : { KEEP(*(.text.start)) *(.text*) *(.rodata*) } :text
    /DISCARD/ : { *(.eh_frame*) *(.note*) *(.comment*) }
}
