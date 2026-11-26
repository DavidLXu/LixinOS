// Common AArch64 EL0 execution lane. System encodings also assemble with
// the native TinyCC assembler. The primary and secondary use the same frame.
#define REG(a,b,c,d,e) (((a)<<14)|((b)<<11)|((c)<<7)|((d)<<3)|(e))
#define RD(x,r) .long (0xd5300000|((r)<<5)|(x))
#define WR(r,x) .long (0xd5100000|((r)<<5)|(x))
#define TTBR REG(3,0,2,0,0)
#define MAIR REG(3,0,10,2,0)
#define TCR REG(3,0,2,0,2)
#define SCTLR REG(3,0,1,0,0)
#define CPACR REG(3,0,1,0,2)
#define CNTKCTL REG(3,0,14,1,0)
#define VBAR REG(3,0,12,0,0)
#define ELR REG(3,0,4,0,1)
#define SPSR REG(3,0,4,0,0)
#define USP REG(3,0,4,1,0)
#define TLS REG(3,3,13,0,2)
#define TVAL REG(3,3,14,3,0)
#define TCTL REG(3,3,14,3,1)
#define PUSHQ(a,b) .long (0xadbf0000 | ((b)<<10) | (31<<5) | (a))
#define POPQ(a,b) .long (0xacc10000 | ((b)<<10) | (31<<5) | (a))
.text
.global smp_power_on
smp_power_on:
    RD(9,TTBR)
    RD(10,MAIR)
    stp x9,x10,[x0]
    RD(9,TCR)
    RD(10,SCTLR)
    stp x9,x10,[x0,#16]
    RD(9,CPACR)
    RD(10,CNTKCTL)
    // EL0 may read the virtual clock, but cannot program the timer.
    mov x11,#2
    orr x10,x10,x11
    WR(CNTKCTL,10)
    isb
    stp x9,x10,[x0,#32]
    RD(9,VBAR)
    str x9,[x0,#48]
    mov x3,x0
    mov x1,#1
    movz x0,#0xc400,lsl #16
    movk x0,#3
    mov x12,x30
    bl smp_psci
    .quad smp_entry
smp_psci:
    ldr x2,[x30]
    mov x30,x12
    dsb sy
    .long 0xd4000002 // hvc PSCI CPU_ON
    ret
smp_entry:
    .long 0xd5034fdf // mask exceptions
    ldp x9,x10,[x0]
    WR(TTBR,9)
    WR(MAIR,10)
    ldp x9,x10,[x0,#16]
    WR(TCR,9)
    dsb sy
    isb
    .long 0xd508871f // tlbi vmalle1
    dsb sy
    isb
    WR(SCTLR,10)
    isb
    ldp x9,x10,[x0,#32]
    WR(CPACR,9)
    WR(CNTKCTL,10)
    ldr x10,[x0,#48]
    WR(VBAR,10)
    bl smp_start_stack
    .quad smp_stack_top
smp_start_stack:
    ldr x9,[x30]
    mov sp,x9
    isb
    b smp_secondary_main
.global smp_irq_vector
smp_irq_vector:
    stp x0,x1,[sp,#-16]!
    stp x2,x3,[sp,#-16]!
    stp x4,x5,[sp,#-16]!
    stp x6,x7,[sp,#-16]!
    stp x8,x9,[sp,#-16]!
    stp x10,x11,[sp,#-16]!
    stp x12,x13,[sp,#-16]!
    stp x14,x15,[sp,#-16]!
    stp x16,x17,[sp,#-16]!
    stp x18,x19,[sp,#-16]!
    stp x20,x21,[sp,#-16]!
    stp x22,x23,[sp,#-16]!
    stp x24,x25,[sp,#-16]!
    stp x26,x27,[sp,#-16]!
    stp x28,x29,[sp,#-16]!
    stp x30,xzr,[sp,#-16]!
    PUSHQ(0,1)
    PUSHQ(2,3)
    PUSHQ(4,5)
    PUSHQ(6,7)
    PUSHQ(8,9)
    PUSHQ(10,11)
    PUSHQ(12,13)
    PUSHQ(14,15)
    PUSHQ(16,17)
    PUSHQ(18,19)
    PUSHQ(20,21)
    PUSHQ(22,23)
    PUSHQ(24,25)
    PUSHQ(26,27)
    PUSHQ(28,29)
    PUSHQ(30,31)

    mrs x9,fpcr
    mrs x10,fpsr
    stp x9,x10,[sp,#-16]!
    RD(9,USP)
    RD(10,TLS)
    stp x9,x10,[sp,#-16]!
    RD(9,ELR)
    RD(10,SPSR)
    stp x9,x10,[sp,#-16]!
    bl smp_kernel_enter
    mov x0,sp
    bl smp_timer_irq
    mov x0,sp
    bl guest_irq_check
    cbnz x0,guest_unwind_to_host
    bl smp_check_stop
    cbz x0,smp_restore
    add sp,sp,#816
    b smp_abort_user
smp_restore:
    ldp x9,x10,[sp],#16
    WR(ELR,9)
    WR(SPSR,10)
    ldp x9,x10,[sp],#16
    WR(USP,9)
    WR(TLS,10)
    ldp x9,x10,[sp],#16
    msr fpcr,x9
    msr fpsr,x10
    POPQ(30,31)
    POPQ(28,29)
    POPQ(26,27)
    POPQ(24,25)
    POPQ(22,23)
    POPQ(20,21)
    POPQ(18,19)
    POPQ(16,17)
    POPQ(14,15)
    POPQ(12,13)
    POPQ(10,11)
    POPQ(8,9)
    POPQ(6,7)
    POPQ(4,5)
    POPQ(2,3)
    POPQ(0,1)
    ldp x30,xzr,[sp],#16
    ldp x28,x29,[sp],#16
    ldp x26,x27,[sp],#16
    ldp x24,x25,[sp],#16
    ldp x22,x23,[sp],#16
    ldp x20,x21,[sp],#16
    ldp x18,x19,[sp],#16
    ldp x16,x17,[sp],#16
    ldp x14,x15,[sp],#16
    ldp x12,x13,[sp],#16
    ldp x10,x11,[sp],#16
    ldp x8,x9,[sp],#16
    ldp x6,x7,[sp],#16
    ldp x4,x5,[sp],#16
    ldp x2,x3,[sp],#16
    ldp x0,x1,[sp],#16

    b smp_leave_user
.global smp_enter_preserved
smp_enter_preserved:
    stp x0,x1,[sp,#-16]!
    stp x2,x3,[sp,#-16]!
    stp x4,x5,[sp,#-16]!
    stp x6,x7,[sp,#-16]!
    stp x8,x9,[sp,#-16]!
    stp x10,x11,[sp,#-16]!
    stp x12,x13,[sp,#-16]!
    stp x14,x15,[sp,#-16]!
    stp x16,x17,[sp,#-16]!
    stp x18,x30,[sp,#-16]!
    add x0,sp,#144 // x0 saved above nine GPR pairs
    mov x1,x8
    RD(2,REG(3,0,5,2,0))
    bl smp_enter_svc
    .long 0xf100001f // cmp x0,#0
    ldp x18,x30,[sp],#16
    ldp x16,x17,[sp],#16
    ldp x14,x15,[sp],#16
    ldp x12,x13,[sp],#16
    ldp x10,x11,[sp],#16
    ldp x8,x9,[sp],#16
    ldp x6,x7,[sp],#16
    ldp x4,x5,[sp],#16
    ldp x2,x3,[sp],#16
    ldp x0,x1,[sp],#16
    bne smp_clock_return
    ret
smp_clock_return:
    ldp x30,xzr,[sp],#16
    .long 0xd69f03e0 // EL0, no kernel ticket was taken
// Allocate a ticket atomically. EL1 IRQs are masked while a ticket is held.
.global smp_take_ticket
smp_take_ticket:
    .long 0xc85f7c01 // ldxr x1,[x0]
    add x2,x1,#1
    .long 0xc8037c02 // stxr w3,x2,[x0]
    cbnz w3,smp_take_ticket
    mov x0,x1
    ret
.global smp_cpu
smp_cpu:
    RD(0,REG(3,0,0,0,5))
    and x0,x0,#1
    ret
.global smp_leave_user
smp_leave_user:
    stp x9,x10,[sp,#-16]!
    stp x30,xzr,[sp,#-16]!
    bl smp_leave_address
    .quad smp_serving
smp_leave_address:
    ldr x9,[x30]
    ldr x10,[x9]
    add x10,x10,#1
    .long 0xc89ffd2a // stlr x10,[x9]
    dsb ish
    sev
    ldp x30,xzr,[sp],#16
    ldp x9,x10,[sp],#16
    .long 0xd69f03e0 // eret
.global smp_acquire
smp_acquire:
    .long 0xc8dffc00 // ldar x0,[x0]
    ret
.global smp_release
smp_release:
    .long 0xc89ffc01 // stlr x1,[x0]
    ret
.global smp_root
smp_root:
    dsb ish
    WR(TTBR,0)
    isb
    .long 0xd508871f // tlbi vmalle1
    dsb ish
    .long 0xd508751f // ic iallu
    dsb ish
    isb
    ret
.global smp_ticks
smp_ticks:
    RD(0,REG(3,3,14,0,2))
    ret
.global smp_frequency
smp_frequency:
    RD(0,REG(3,3,14,0,0))
    ret
.global smp_timer
smp_timer:
    WR(TVAL,0)
    mov x0,#1
    WR(TCTL,0)
    isb
    ret
.global smp_gic3_enable
smp_gic3_enable:
    RD(0,REG(3,0,12,12,5))
    mov x1,#1
    orr x0,x0,x1
    WR(REG(3,0,12,12,5),0)
    isb
    mov x0,#255
    WR(REG(3,0,4,6,0),0)
    WR(REG(3,0,12,12,7),1)
    isb
    ret
.bss
.balign 16
    .skip 65536
smp_stack_top:

.text
.global kt_access
kt_access:
    cbz x1,kt_read_access
    .long 0xd5087860 // at s1e0w,x0
    b kt_access_result
kt_read_access:
    .long 0xd5087840 // at s1e0r,x0
kt_access_result:
    isb
    RD(0,REG(3,0,7,4,0))
    and x0,x0,#1
    eor x0,x0,#1
    ret

.global kt_fpcr
kt_fpcr:
    mrs x0,fpcr
    ret
.global kt_fpsr
kt_fpsr:
    mrs x0,fpsr
    ret

// Start with the creator's FP environment and clean vector registers.
.global kt_init_fp
kt_init_fp:
    msr fpcr,x4
    msr fpsr,x5
    .long 0x4f00e400 // movi v0.16b,#0
    .long 0x4f00e401 // movi v1.16b,#0
    .long 0x4f00e402 // movi v2.16b,#0
    .long 0x4f00e403 // movi v3.16b,#0
    .long 0x4f00e404 // movi v4.16b,#0
    .long 0x4f00e405 // movi v5.16b,#0
    .long 0x4f00e406 // movi v6.16b,#0
    .long 0x4f00e407 // movi v7.16b,#0
    .long 0x4f00e408 // movi v8.16b,#0
    .long 0x4f00e409 // movi v9.16b,#0
    .long 0x4f00e40a // movi v10.16b,#0
    .long 0x4f00e40b // movi v11.16b,#0
    .long 0x4f00e40c // movi v12.16b,#0
    .long 0x4f00e40d // movi v13.16b,#0
    .long 0x4f00e40e // movi v14.16b,#0
    .long 0x4f00e40f // movi v15.16b,#0
    .long 0x4f00e410 // movi v16.16b,#0
    .long 0x4f00e411 // movi v17.16b,#0
    .long 0x4f00e412 // movi v18.16b,#0
    .long 0x4f00e413 // movi v19.16b,#0
    .long 0x4f00e414 // movi v20.16b,#0
    .long 0x4f00e415 // movi v21.16b,#0
    .long 0x4f00e416 // movi v22.16b,#0
    .long 0x4f00e417 // movi v23.16b,#0
    .long 0x4f00e418 // movi v24.16b,#0
    .long 0x4f00e419 // movi v25.16b,#0
    .long 0x4f00e41a // movi v26.16b,#0
    .long 0x4f00e41b // movi v27.16b,#0
    .long 0x4f00e41c // movi v28.16b,#0
    .long 0x4f00e41d // movi v29.16b,#0
    .long 0x4f00e41e // movi v30.16b,#0
    .long 0x4f00e41f // movi v31.16b,#0
    ret

.global vm_current_root
vm_current_root:
    RD(0,TTBR)
    ret
.global vm_retry_user
vm_retry_user:
    stp x0,x1,[sp,#-16]!
    stp x2,x3,[sp,#-16]!
    stp x4,x5,[sp,#-16]!
    stp x6,x7,[sp,#-16]!
    stp x8,x9,[sp,#-16]!
    stp x10,x11,[sp,#-16]!
    stp x12,x13,[sp,#-16]!
    stp x14,x15,[sp,#-16]!
    stp x16,x17,[sp,#-16]!
    stp x18,x19,[sp,#-16]!
    stp x20,x21,[sp,#-16]!
    stp x22,x23,[sp,#-16]!
    stp x24,x25,[sp,#-16]!
    stp x26,x27,[sp,#-16]!
    stp x28,x29,[sp,#-16]!
    stp x30,xzr,[sp,#-16]!
    PUSHQ(0,1)
    PUSHQ(2,3)
    PUSHQ(4,5)
    PUSHQ(6,7)
    PUSHQ(8,9)
    PUSHQ(10,11)
    PUSHQ(12,13)
    PUSHQ(14,15)
    PUSHQ(16,17)
    PUSHQ(18,19)
    PUSHQ(20,21)
    PUSHQ(22,23)
    PUSHQ(24,25)
    PUSHQ(26,27)
    PUSHQ(28,29)
    PUSHQ(30,31)
    RD(0,REG(3,0,5,2,0))
    RD(1,REG(3,0,6,0,0))
    bl vm_handle_fault
    cbnz x0,vm_retry_done
    RD(0,REG(3,0,5,2,0))
    RD(1,REG(3,0,6,0,0))
    bl guest_handle_fault
vm_retry_done:
    .long 0xf100001f // cmp x0,#0
    POPQ(30,31)
    POPQ(28,29)
    POPQ(26,27)
    POPQ(24,25)
    POPQ(22,23)
    POPQ(20,21)
    POPQ(18,19)
    POPQ(16,17)
    POPQ(14,15)
    POPQ(12,13)
    POPQ(10,11)
    POPQ(8,9)
    POPQ(6,7)
    POPQ(4,5)
    POPQ(2,3)
    POPQ(0,1)
    ldp x30,xzr,[sp],#16
    ldp x28,x29,[sp],#16
    ldp x26,x27,[sp],#16
    ldp x24,x25,[sp],#16
    ldp x22,x23,[sp],#16
    ldp x20,x21,[sp],#16
    ldp x18,x19,[sp],#16
    ldp x16,x17,[sp],#16
    ldp x14,x15,[sp],#16
    ldp x12,x13,[sp],#16
    ldp x10,x11,[sp],#16
    ldp x8,x9,[sp],#16
    ldp x6,x7,[sp],#16
    ldp x4,x5,[sp],#16
    ldp x2,x3,[sp],#16
    ldp x0,x1,[sp],#16
    beq vm_unhandled
    ldp x30,xzr,[sp],#16
    b smp_leave_user
vm_unhandled:
    ret
.global vm_sync
vm_sync:
    dsb ishst
    .long 0xd508831f // tlbi vmalle1is
    dsb ish
    isb
    ret

.global vm_sync_code
vm_sync_code:
    // Minimum cache line on AArch64 is 16 bytes.
    add x1,x0,#4096
vm_clean_line:
    .long 0xd50b7b20 // dc cvau,x0
    add x0,x0,#16
    .long 0xeb01001f // cmp x0,x1
    bne vm_clean_line
    dsb ish
    .long 0xd508711f // ic ialluis
    dsb ish
    isb
    ret
