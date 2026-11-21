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
    .eh_frame : { KEEP(*(.eh_frame)) } :text
    .init : { KEEP(*(.init)) } :text
    .fini : { KEEP(*(.fini)) } :text
    . = ALIGN(0x200000);
    .data : { *(.data*) *(.got*) } :data
    .init_array : {
        __init_array_start = .; KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*))) KEEP(*(.init_array)) __init_array_end = .;
    } :data
    .fini_array : {
        __fini_array_start = .; KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*))) KEEP(*(.fini_array)) __fini_array_end = .;
    } :data
    .tdata : { *(.tdata*) } :data :tls
    .tbss : { *(.tbss*) } :data :tls
    .bss : { *(.bss*) *(COMMON) } :data
    __end_image = ALIGN(4096);
    /DISCARD/ : { *(.comment*) *(.note*) }
}
