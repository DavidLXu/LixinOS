ENTRY(_start)
SECTIONS {
    . = 0x44000000;
    .text : { KEEP(*(.text.start)) *(.text*) *(.rodata*) }
    ASSERT(. <= 0x44170000, "user code overlaps stdio ABI")
    .lxstdio 0x44170000 : { KEEP(*(.lxstdio)) }
    /* The bootstrap compiler receives source in a dedicated data buffer. */
    PROVIDE(__bootstrap_input_bytes = 0);
    . = 0x44200000 + __bootstrap_input_bytes;
    .data : { *(.data*) *(.got*) }
    .bss (NOLOAD) : { *(.bss*) *(COMMON) }
    __user_heap_start = ALIGN(16);
    PROVIDE(__user_heap_limit = 0x443d8000);
    ASSERT(__user_heap_start <= __user_heap_limit, "runtime overlaps program data")
    ASSERT(. <= 0x443d8000, "user data overlaps script buffer or stack")
    /DISCARD/ : { *(.eh_frame*) *(.note*) *(.comment*) }
}
