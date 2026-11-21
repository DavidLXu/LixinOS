.section .text.start,"ax"
.global _start
_start:
    cmp x3, #65536
    b.ne lx_app_start
    adrp x9, desktop_thread_pointer
    ldr x9, [x9, :lo12:desktop_thread_pointer]
    msr tpidr_el0, x9
    b desktop_resume
