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
    mov x9, #0x340
    msr spsr_el1, x9
    b smp_leave_user

.balign 2048
.global user_vectors
user_vectors:
.rept 8
    b kernel_vector_fault
    .balign 128
.endr
    b user_sync
    .balign 128
    b smp_irq_vector
    .balign 128
.rept 6
    b kernel_vector_fault
    .balign 128
.endr

user_sync:
    stp x30,xzr,[sp,#-16]!
    bl smp_enter_preserved
    ldp x30,xzr,[sp],#16
    mrs x9, esr_el1
    lsr x10, x9, #26
    cmp x10, #0x15
    b.ne user_fault
    cmp x8, #2
    b.eq resume_shell
    cmp x8, #32
    b.eq suspend_for_child
    cmp x8, #60
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
    cmn x0, #4095
    b.ne 3f
    mov x0, #130
    b resume_shell
3:  cmn x0, #4094
    b.ne 4f
    mov x0, #137
    b resume_shell
4:
    // A closed desktop terminal unwinds the foreground process at a syscall.
    cmn x0, #1, lsl #12
    b.ne 1f
    mov x0, #0
    b resume_shell
1:
    cmp x8, #6
    b.hs return_to_user
    cmn x0, #2
    b.eq resume_shell
return_to_user:
    b smp_leave_user
suspend_for_child:
    mov x0, #256
    b resume_shell
unknown_call:
    mov x0, #-1
    b smp_leave_user
user_fault:
    mov x0, x9
    mrs x1, elr_el1
    mrs x2, far_el1
    mrs x3, sp_el0
    bl report_user_fault
    b resume_shell
.global resume_shell
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
