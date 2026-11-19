ENTRY(_start)
PHDRS {
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
    tls PT_TLS FLAGS(4);
}
SECTIONS {
    . = 0x44000000;
    .text : {
        KEEP(*(.text.start))
        *(.text*) *(.rodata*) *(.eh_frame*)
        . = ALIGN(8);
        lx_tls_phdr = .;
        LONG(7) LONG(4) QUAD(0) QUAD(ADDR(.tdata)) QUAD(0)
        QUAD(SIZEOF(.tdata)) QUAD(__tls_end - ADDR(.tdata))
        /* Match PT_TLS exactly so musl and ld agree on offsets from TP. */
        QUAD(MAX(1, MAX(ALIGNOF(.tdata), ALIGNOF(.tbss))))
    } :text
    ASSERT(. <= 0x44200000, "program exceeds LixinOS executable text bank")
    . = 0x44200000;
    .data : { *(.data*) *(.got*) } :data
    .init_array : {
        __init_array_start = .; KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*))) KEEP(*(.init_array)) __init_array_end = .;
    } :data
    .fini_array : {
        __fini_array_start = .; KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*))) KEEP(*(.fini_array)) __fini_array_end = .;
    } :data
    .tdata : { *(.tdata*) } :data :tls
    .tbss : { *(.tbss*) } :data :tls
    __tls_end = ADDR(.tbss) + SIZEOF(.tbss);
    .bss : { *(.bss*) *(COMMON) } :data
    ASSERT(. <= 0x443d7000, "program exceeds LixinOS writable data bank")
    __end_image = 0x78000000;
    .rustc 0 (INFO) : { KEEP(*(.rustc)) } :NONE
    /DISCARD/ : { *(.note*) *(.comment*) }
}
