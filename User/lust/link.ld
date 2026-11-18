ENTRY(_start)
SECTIONS {
    . = 0x44000000;
    .text : { KEEP(*(.text.start)) *(.text*) *(.rodata*) }
    __data_image = ALIGN(16);
    .data 0x44200000 : AT(__data_image) {
        __data_begin = .; *(.data*) *(.got*) __data_end = .;
    }
    .bss (NOLOAD) : { *(.bss*) *(COMMON) }
    __user_heap_start = ALIGN(16);
    __user_heap_limit = 0x443d7000;
    ASSERT(. <= __user_heap_limit, "compiler overlaps utility mailbox")
    /DISCARD/ : { *(.eh_frame*) *(.note*) *(.comment*) }
}
