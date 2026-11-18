ENTRY(_start)

SECTIONS
{
    /* QEMU virt puts its device tree at 0x40000000 for an ELF boot. */
    . = 0x40200000;
    __kernel_start = .;

    .text : {
        KEEP(*(.text.boot))
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) }

    .stack ALIGN(16) (NOLOAD) : {
        /* The desktop can host a shell and its foreground child on this stack. */
        __stack_bottom = .;
        . += 128K;
        __stack_top = .;
    }
}

ASSERT(__stack_top <= 0x44000000, "kernel overlaps user image")
