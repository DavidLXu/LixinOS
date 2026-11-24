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

.global smp_abort_user
smp_abort_user:
    b resume_shell
.global smp_check_stop
smp_check_stop:
    b user_interrupt_status
