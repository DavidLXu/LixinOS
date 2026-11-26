/* Immutable AArch64 entry points for the static UNO C++ bridge. Each entry
   selects its pair of function index and base-interface offset. */
.section .text.lx_writer_uno_slots, "ax", %progbits
.p2align 3
.global lx_writer_uno_slots
.hidden lx_writer_uno_slots
lx_writer_uno_slots:
.set slot, 0
.rept 8192
    mov w9, #slot
    b .Ldispatch
    .set slot, slot+1
.endr
.Ldispatch:
    adrp x11, lx_writer_uno_arguments
    add x11, x11, :lo12:lx_writer_uno_arguments
    add x11, x11, x9, lsl #3
    ldp w9, w10, [x11]
    b vtableSlotCall
.section .note.GNU-stack,"",%progbits
