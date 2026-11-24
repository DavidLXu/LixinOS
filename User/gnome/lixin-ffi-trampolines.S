#include "../lixin-ffi.h"

    .text
    .p2align 5
    .global lx_ffi_trampoline_table
    .hidden lx_ffi_trampoline_table
    .hidden lx_ffi_slots
    .type lx_ffi_trampoline_table, %function
lx_ffi_trampoline_table:
    .set slot, 0
    .rept LX_FFI_SLOTS
    .inst 0xd503245f  // bti c
    adrp x17, lx_ffi_slots + 16 * slot
    add x17, x17, :lo12:lx_ffi_slots + 16 * slot
    ldar x16, [x17]   // closure entry point
    ldr x17, [x17, #8] // ffi_closure pointer expected by ffi_closure_SYSV
    br x16
    nop
    nop
    .set slot, slot + 1
    .endr
    .size lx_ffi_trampoline_table, . - lx_ffi_trampoline_table
