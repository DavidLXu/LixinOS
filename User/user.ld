ENTRY(_start)
SECTIONS {
    . = 0x44000000;
    .text : { KEEP(*(.text.start)) *(.text*) *(.rodata*) }
    ASSERT(. <= 0x44170000, "user code overlaps stdio ABI")
    .lxstdio 0x44170000 : { KEEP(*(.lxstdio)) }
    . = 0x44200000;
    .data : { *(.data*) *(.got*) }
    .bss (NOLOAD) : { *(.bss*) *(COMMON) }
    __user_heap_start = ALIGN(16);
    PROVIDE(__user_heap_limit = 0x443d8000);
    ASSERT(__user_heap_start <= __user_heap_limit, "runtime overlaps program data")
    ASSERT(. <= 0x443d8000, "user data overlaps script buffer or stack")
    /DISCARD/ : { *(.eh_frame*) *(.note*) *(.comment*) }
}
