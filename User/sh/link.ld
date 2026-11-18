ENTRY(_start)
PHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
    . = 0x44000000;
    .text : { KEEP(*(.text.start)) *(.text*) *(.rodata*) } :text
    . = 0x44200000;
    .data : { *(.data*) *(.got*) } :data
    .bss (NOLOAD) : { *(.bss*) *(COMMON) } :data
    __user_heap_start = ALIGN(16);
    __user_heap_limit = 0x443d7000;
    ASSERT(. <= __user_heap_limit, "shell overlaps mailbox")
    /DISCARD/ : { *(.eh_frame*) *(.note*) *(.comment*) }
}
