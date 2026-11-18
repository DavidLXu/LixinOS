// One foreground EL0 context. The shell's suspended EL1 frame stays on SP_EL1.
.global enter_user
enter_user:
    stp x29, x30, [sp, #-16]!
    stp x19, x20, [sp, #-16]!
    stp x21, x22, [sp, #-16]!
    stp x23, x24, [sp, #-16]!
    stp x25, x26, [sp, #-16]!
    stp x27, x28, [sp, #-16]!
    mov x9, #0x44000000
    msr elr_el1, x9
    mov x9, #0x44400000
    msr sp_el0, x9
    .if {split}
    mov x9, #0x340
    .else
    mov x9, #0x3c0
    .endif
    msr spsr_el1, x9
    eret

.balign 2048
.global user_vectors
user_vectors:
.rept 8
    b kernel_vector_fault
    .balign 128
.endr
    b user_sync
    .balign 128
    .if {split}
    b user_timer_irq
    .else
    b kernel_vector_fault
    .endif
    .balign 128
.rept 6
    b kernel_vector_fault
    .balign 128
.endr

user_sync:
    mrs x9, esr_el1
    lsr x10, x9, #26
    cmp x10, #0x15
    b.ne user_fault
    cmp x8, #2
    b.eq resume_shell
    cmp x8, #32
    b.eq suspend_for_child
    cmp x8, #31
    b.hi unknown_call
    // The kernel uses the soft-float target (no SIMD registers). Preserve
    // the user integer context across the shared Rust console implementation.
    stp x1, x2, [sp, #-16]!
    stp x3, x4, [sp, #-16]!
    stp x5, x6, [sp, #-16]!
    stp x7, x8, [sp, #-16]!
    stp x9, x10, [sp, #-16]!
    stp x11, x12, [sp, #-16]!
    stp x13, x14, [sp, #-16]!
    stp x15, x16, [sp, #-16]!
    stp x17, x18, [sp, #-16]!
    stp x29, x30, [sp, #-16]!
    mov x1, x8
    bl console_io
    ldp x29, x30, [sp], #16
    ldp x17, x18, [sp], #16
    ldp x15, x16, [sp], #16
    ldp x13, x14, [sp], #16
    ldp x11, x12, [sp], #16
    ldp x9, x10, [sp], #16
    ldp x7, x8, [sp], #16
    ldp x5, x6, [sp], #16
    ldp x3, x4, [sp], #16
    ldp x1, x2, [sp], #16
    cmp x8, #6
    b.hs return_to_user
    cmn x0, #2
    b.eq resume_shell
return_to_user:
    eret
suspend_for_child:
    mov x0, #256
    b resume_shell
unknown_call:
    mov x0, #-1
    eret
user_fault:
    mov x0, #-1
resume_shell:
    ldp x27, x28, [sp], #16
    ldp x25, x26, [sp], #16
    ldp x23, x24, [sp], #16
    ldp x21, x22, [sp], #16
    ldp x19, x20, [sp], #16
    ldp x29, x30, [sp], #16
    // ERET clears the exception state and resumes the caller at EL1h.
    msr elr_el1, x30
    mov x9, #0x3c5
    msr spsr_el1, x9
    eret
