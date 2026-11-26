ENTRY(_start)
PHDRS {
    headers PT_PHDR PHDRS;
    text PT_LOAD FILEHDR PHDRS FLAGS(5);
    data PT_LOAD FLAGS(6);
    tls PT_TLS FLAGS(4);
}
SECTIONS {
    . = 0x78000000 + SIZEOF_HEADERS;
    .text : { *(.text*) *(.rodata*) *(.gcc_except_table*) } :text
    /* Archive members may follow crtend.o on a static link. Register their
       unwind records before the terminator emitted by crtend.o. */
    .eh_frame : {
        KEEP(*crtbegin.o(.eh_frame))
        KEEP(*(EXCLUDE_FILE (*crtend.o) .eh_frame))
        KEEP(*crtend.o(.eh_frame))
    } :text
    . = ALIGN(0x200000);
    .data : { *(.data*) *(.got*) } :data
    .init_array : {
        __init_array_start = .; KEEP(*(.init_array*)) __init_array_end = .;
    } :data
    .fini_array : {
        __fini_array_start = .; KEEP(*(.fini_array*)) __fini_array_end = .;
    } :data
    .tdata : { *(.tdata*) } :data :tls
    .tbss : { *(.tbss*) } :data :tls
    .bss : { *(.bss*) *(COMMON) } :data
    __end_image = ALIGN(4096);
    /DISCARD/ : { *(.comment*) *(.note*) }
}
