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
    stp x9,x10,[x0,#32]
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
    bl smp_start_stack
    .quad smp_stack_top
    .quad smp_vectors
smp_start_stack:
    ldp x9,x10,[x30]
    mov sp,x9
    WR(VBAR,10)
    isb
    b smp_secondary_main
.balign 2048
smp_vectors:
.rept 8
    b smp_fatal
    .balign 128
.endr
    b smp_sync
    .balign 128
    b smp_irq
    .balign 128
.rept 6
    b smp_fatal
    .balign 128
.endr
smp_fatal:
    wfe
    b smp_fatal
smp_sync:
    stp x0,x1,[sp,#-16]!
    mov x0,#1
    b smp_save
smp_irq:
    stp x0,x1,[sp,#-16]!
    mov x0,#2
    b smp_save
.global smp_primary_vector
smp_primary_vector:
    stp x0,x1,[sp,#-16]!
    mov x0,#0
smp_save:
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
    cbnz x0,smp_remote_trap
    mov x0,sp
    bl smp_primary_tick
    bl smp_check_stop
    cbz x0,smp_restore
    add sp,sp,#816
    b smp_abort_user
smp_remote_trap:
    RD(1,REG(3,0,5,2,0))
    bl smp_trap_stack
    .quad smp_stack_top
smp_trap_stack:
    ldr x9,[x30]
    mov sp,x9
    b smp_secondary_trap
.global smp_resume
smp_resume:
    mov x19,x0
    mov x0,x1
    bl smp_root
    mov x0,x19
.global smp_continue
smp_continue:
    mov sp,x0
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

    .long 0xd69f03e0 // eret
.global smp_acquire
smp_acquire:
    .long 0xc8dffc00 // ldar x0,[x0]
    ret
.global smp_release
smp_release:
    .long 0xc89ffc01 // stlr x1,[x0]
    ret
.global smp_current_root
smp_current_root:
    RD(0,TTBR)
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
.global smp_timer_off
smp_timer_off:
    mov x0,#0
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
.global smp_gic3_ack
smp_gic3_ack:
    RD(0,REG(3,0,12,12,0))
    ret
.global smp_gic3_eoi
smp_gic3_eoi:
    WR(REG(3,0,12,12,1),0)
    isb
    ret
.bss
.balign 16
    .skip 16384
smp_stack_top:
