// libstdc++ was compiled for musl's native TLS; guest C++ objects use emutls.
// Return the native slots consumed by libstdc++'s __once_proxy.
.text
.macro once_slot function, slot
.global \function
.type \function, %function
\function:
    mrs x0, tpidr_el0
    add x0, x0, #:tprel_hi12:\slot
    add x0, x0, #:tprel_lo12_nc:\slot
    ret
.size \function, .-\function
.endm
once_slot lx_once_call, _ZSt11__once_call
once_slot lx_once_callable, _ZSt15__once_callable
.section .note.GNU-stack,"",%progbits
