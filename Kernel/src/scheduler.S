.arch armv8-a+simd
.global swap_stack
swap_stack:
    // Rust's soft-float kernel follows AAPCS64. Preserve its callee-saved
    // registers, plus all FP/SIMD and exception state belonging to EL0.
    stp x19, x20, [sp, #-16]!
    stp x21, x22, [sp, #-16]!
    stp x23, x24, [sp, #-16]!
    stp x25, x26, [sp, #-16]!
    stp x27, x28, [sp, #-16]!
    stp x29, x30, [sp, #-16]!
    stp q0, q1, [sp, #-32]!
    stp q2, q3, [sp, #-32]!
    stp q4, q5, [sp, #-32]!
    stp q6, q7, [sp, #-32]!
    stp q8, q9, [sp, #-32]!
    stp q10, q11, [sp, #-32]!
    stp q12, q13, [sp, #-32]!
    stp q14, q15, [sp, #-32]!
    stp q16, q17, [sp, #-32]!
    stp q18, q19, [sp, #-32]!
    stp q20, q21, [sp, #-32]!
    stp q22, q23, [sp, #-32]!
    stp q24, q25, [sp, #-32]!
    stp q26, q27, [sp, #-32]!
    stp q28, q29, [sp, #-32]!
    stp q30, q31, [sp, #-32]!
    mrs x9, elr_el1
    mrs x10, spsr_el1
    stp x9, x10, [sp, #-16]!
    mrs x9, fpcr
    mrs x10, fpsr
    stp x9, x10, [sp, #-16]!
    mrs x9, sp_el0
    mrs x10, tpidr_el0
    stp x9, x10, [sp, #-16]!
    mov x9, sp
    str x9, [x0]
    cbnz x1, 1f
    mov sp, x2
    b start_task
1:
    mov sp, x1
    ldp x9, x10, [sp], #16
    msr tpidr_el0, x10
    msr sp_el0, x9
    ldp x9, x10, [sp], #16
    msr fpcr, x9
    msr fpsr, x10
    ldp x9, x10, [sp], #16
    msr elr_el1, x9
    msr spsr_el1, x10
    ldp q30, q31, [sp], #32
    ldp q28, q29, [sp], #32
    ldp q26, q27, [sp], #32
    ldp q24, q25, [sp], #32
    ldp q22, q23, [sp], #32
    ldp q20, q21, [sp], #32
    ldp q18, q19, [sp], #32
    ldp q16, q17, [sp], #32
    ldp q14, q15, [sp], #32
    ldp q12, q13, [sp], #32
    ldp q10, q11, [sp], #32
    ldp q8, q9, [sp], #32
    ldp q6, q7, [sp], #32
    ldp q4, q5, [sp], #32
    ldp q2, q3, [sp], #32
    ldp q0, q1, [sp], #32
    ldp x29, x30, [sp], #16
    ldp x27, x28, [sp], #16
    ldp x25, x26, [sp], #16
    ldp x23, x24, [sp], #16
    ldp x21, x22, [sp], #16
    ldp x19, x20, [sp], #16
    ret

.global user_timer_irq
user_timer_irq:
    // An interrupt may land between any two user instructions.
    stp x0, x1, [sp, #-16]!
    stp x2, x3, [sp, #-16]!
    stp x4, x5, [sp, #-16]!
    stp x6, x7, [sp, #-16]!
    stp x8, x9, [sp, #-16]!
    stp x10, x11, [sp, #-16]!
    stp x12, x13, [sp, #-16]!
    stp x14, x15, [sp, #-16]!
    stp x16, x17, [sp, #-16]!
    stp x18, x19, [sp, #-16]!
    stp x20, x21, [sp, #-16]!
    stp x22, x23, [sp, #-16]!
    stp x24, x25, [sp, #-16]!
    stp x26, x27, [sp, #-16]!
    stp x28, x29, [sp, #-16]!
    stp x30, xzr, [sp, #-16]!
    bl task_timer_irq
    bl user_interrupt_status
    cbnz w0, 2f
    ldp x30, xzr, [sp], #16
    ldp x28, x29, [sp], #16
    ldp x26, x27, [sp], #16
    ldp x24, x25, [sp], #16
    ldp x22, x23, [sp], #16
    ldp x20, x21, [sp], #16
    ldp x18, x19, [sp], #16
    ldp x16, x17, [sp], #16
    ldp x14, x15, [sp], #16
    ldp x12, x13, [sp], #16
    ldp x10, x11, [sp], #16
    ldp x8, x9, [sp], #16
    ldp x6, x7, [sp], #16
    ldp x4, x5, [sp], #16
    ldp x2, x3, [sp], #16
    ldp x0, x1, [sp], #16
    eret
2:
    add sp, sp, #256
    b resume_shell
