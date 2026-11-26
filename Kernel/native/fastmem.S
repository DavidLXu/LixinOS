// TinyCC-compatible AArch64 bulk memory and framebuffer helpers.
// Uses only 64-bit base+offset instructions proven in el1.S / guest-arch.S.
.text

// void fast_memcpy64(void *dst, const void *src, unsigned long blocks64);
.global fast_memcpy64
fast_memcpy64:
    cbz x2, fast_memcpy64_done
fast_memcpy64_loop:
    ldp x3, x4, [x1, #0]
    ldp x5, x6, [x1, #16]
    ldp x7, x8, [x1, #32]
    ldp x9, x10, [x1, #48]
    stp x3, x4, [x0, #0]
    stp x5, x6, [x0, #16]
    stp x7, x8, [x0, #32]
    stp x9, x10, [x0, #48]
    add x1, x1, #64
    add x0, x0, #64
    sub x2, x2, #1
    cbnz x2, fast_memcpy64_loop
fast_memcpy64_done:
    ret

// void fast_memset64(void *dst, unsigned long fill, unsigned long blocks64);
.global fast_memset64
fast_memset64:
    cbz x2, fast_memset64_done
fast_memset64_loop:
    stp x1, x1, [x0, #0]
    stp x1, x1, [x0, #16]
    stp x1, x1, [x0, #32]
    stp x1, x1, [x0, #48]
    add x0, x0, #64
    sub x2, x2, #1
    cbnz x2, fast_memset64_loop
fast_memset64_done:
    ret

// void pixel_double_pairs(unsigned int *dst, const unsigned int *src, unsigned long pairs);
.global pixel_double_pairs
pixel_double_pairs:
    cbz x2, pixel_double_done
    movz x6, #0xffff
    movk x6, #0xffff, lsl #16
pixel_double_loop:
    ldr x3, [x1]
    and x4, x3, x6
    lsl x5, x4, #32
    orr x4, x4, x5
    lsr x5, x3, #32
    lsl x7, x5, #32
    orr x5, x5, x7
    stp x4, x5, [x0]
    add x1, x1, #8
    add x0, x0, #16
    sub x2, x2, #1
    cbnz x2, pixel_double_loop
pixel_double_done:
    ret
