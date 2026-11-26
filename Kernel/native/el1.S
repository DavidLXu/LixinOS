// Architectural instructions unavailable in the TinyCC A64 mnemonic table.
// Each macro accepts the architectural register encoding (0..30).
#define MSR(reg,xt) .long (0xd5100000 | ((reg)<<5) | (xt))
#define MRS(xt,reg) .long (0xd5300000 | ((reg)<<5) | (xt))
#define SYSREG(op0,op1,crn,crm,op2) (((op0)<<14)|((op1)<<11)|((crn)<<7)|((crm)<<3)|(op2))
#define VBAR SYSREG(3,0,12,0,0)
#define MAIR SYSREG(3,0,10,2,0)
#define TCR SYSREG(3,0,2,0,2)
#define TTBR0 SYSREG(3,0,2,0,0)
#define SCTLR SYSREG(3,0,1,0,0)
#define CPACR SYSREG(3,0,1,0,2)
#define ELR SYSREG(3,0,4,0,1)
#define SPSR SYSREG(3,0,4,0,0)
#define USR_SP SYSREG(3,0,4,1,0)
#define ESR SYSREG(3,0,5,2,0)
#define ERET .long 0xd69f03e0
#define TLBI_ALL .long 0xd508871f
#define IC_ALL .long 0xd508751f
.text
.global enable_mmu
enable_mmu:
    MSR(TTBR0,0)
    MSR(VBAR,1)
    mov x9,#0xff // Normal write-back RAM supports exclusive accesses.
    MSR(MAIR,9)
    mov x9,#0x803019
    lsl x2,x2,#32
    orr x9,x9,x2
    MSR(TCR,9)
    dsb sy
    isb
    TLBI_ALL
    dsb sy
    isb
    MRS(9,SCTLR)
    mov x10,#0x1005 // MMU, data cache, and instruction cache
    orr x9,x9,x10
    MSR(SCTLR,9)
    isb
    MRS(9,CPACR)
    mov x10,#0x300000
    orr x9,x9,x10
    MSR(CPACR,9)
    isb
    ret

.global sync_image
sync_image:
    dsb sy
    .long 0xd508831f // tlbi vmalle1is
    dsb sy
    IC_ALL
    dsb sy
    isb
    ret

.global enter_el0
enter_el0:
    stp x19,x20,[sp,#-16]!
    stp x21,x22,[sp,#-16]!
    stp x23,x24,[sp,#-16]!
    stp x25,x26,[sp,#-16]!
    stp x27,x28,[sp,#-16]!
    stp x29,x30,[sp,#-16]!
    stp d8,d9,[sp,#-16]!
    stp d10,d11,[sp,#-16]!
    stp d12,d13,[sp,#-16]!
    stp d14,d15,[sp,#-16]!
    mov x9,#0x44000000
    MSR(ELR,9)
    mov x9,#0x44400000
    MSR(USR_SP,9)
    mov x9,#0x340
    MSR(SPSR,9)
    b smp_leave_user

.global enter_thread
enter_thread:
    stp x19,x20,[sp,#-16]!
    stp x21,x22,[sp,#-16]!
    stp x23,x24,[sp,#-16]!
    stp x25,x26,[sp,#-16]!
    stp x27,x28,[sp,#-16]!
    stp x29,x30,[sp,#-16]!
    stp d8,d9,[sp,#-16]!
    stp d10,d11,[sp,#-16]!
    stp d12,d13,[sp,#-16]!
    stp d14,d15,[sp,#-16]!
    MSR(ELR,0)
    MSR(USR_SP,1)
    MSR(SYSREG(3,3,13,0,2),2)
    bl kt_init_fp
    mov x0,x3
    mov x1,xzr
    mov x2,xzr
    mov x3,xzr
    mov x4,xzr
    mov x5,xzr
    mov x6,xzr
    mov x7,xzr
    mov x8,xzr
    mov x9,xzr
    mov x10,xzr
    mov x11,xzr
    mov x12,xzr
    mov x13,xzr
    mov x14,xzr
    mov x15,xzr
    mov x16,xzr
    mov x17,xzr
    mov x18,xzr
    mov x19,xzr
    mov x20,xzr
    mov x21,xzr
    mov x22,xzr
    mov x23,xzr
    mov x24,xzr
    mov x25,xzr
    mov x26,xzr
    mov x27,xzr
    mov x28,xzr
    mov x29,xzr
    mov x30,xzr
    mov x9,#0x340
    MSR(SPSR,9)
    b smp_leave_user

.balign 2048
.global native_vectors
native_vectors:
.rept 8
    b fatal_vector
    .balign 128
.endr
    b user_sync
    .balign 128
    b smp_irq_vector
    .balign 128
.rept 6
    b fatal_vector
    .balign 128
.endr
user_sync:
    stp x30,xzr,[sp,#-16]!
    bl smp_enter_preserved
    ldp x30,xzr,[sp],#16
    // Stash scratch and the user return address before examining ESR.
    stp x9,x30,[sp,#-16]!
    MRS(9,ESR)
    lsr x9,x9,#26
    .long 0xf100553f // cmp x9,#0x15
    bne native_vm_fault
    .long 0xf100091f // cmp x8,#2
    beq finish_user
    .long 0xf100811f // cmp x8,#32
    beq spawn_user
    stp x1,x2,[sp,#-16]!
    stp x3,x4,[sp,#-16]!
    stp x5,x6,[sp,#-16]!
    stp x7,x8,[sp,#-16]!
    stp x10,x11,[sp,#-16]!
    stp x12,x13,[sp,#-16]!
    stp x14,x15,[sp,#-16]!
    stp x16,x17,[sp,#-16]!
    stp x18,x19,[sp,#-16]!
    // stp/ldp qA,qB encodings, full SIMD state across the C syscall.
#define PUSHQ(a,b) .long (0xadbf0000 | ((b)<<10) | (31<<5) | (a))
#define POPQ(a,b)  .long (0xacc10000 | ((b)<<10) | (31<<5) | (a))
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
    mov x1,x8
    bl native_syscall
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
    ldp x18,x19,[sp],#16
    ldp x16,x17,[sp],#16
    ldp x14,x15,[sp],#16
    ldp x12,x13,[sp],#16
    ldp x10,x11,[sp],#16
    ldp x7,x8,[sp],#16
    ldp x5,x6,[sp],#16
    ldp x3,x4,[sp],#16
    ldp x1,x2,[sp],#16
    .long 0xb13ffc1f // cmn x0,#4095
    beq interrupted_user
    .long 0xb13ff81f // cmn x0,#4094
    beq stopped_user
    ldp x9,x30,[sp],#16
    b smp_leave_user
interrupted_user:
    mov x0,#130
    b finish_user
stopped_user:
    mov x0,#137
    b finish_user
native_vm_fault:
    ldp x9,x30,[sp],#16
    stp x30,xzr,[sp,#-16]!
    bl vm_retry_user
    ldp x30,xzr,[sp],#16
    stp x9,x30,[sp,#-16]!
bad_user:
    MRS(0,ESR)
    MRS(1,ELR)
    MRS(2,SYSREG(3,0,6,0,0))
    mov x3,x30
    bl report_user_fault
    mov x0,#255
    b finish_user
spawn_user:
    mov x0,#256
finish_user:
    ldp x9,x30,[sp],#16
    ldp d14,d15,[sp],#16
    ldp d12,d13,[sp],#16
    ldp d10,d11,[sp],#16
    ldp d8,d9,[sp],#16
    ldp x29,x30,[sp],#16
    ldp x27,x28,[sp],#16
    ldp x25,x26,[sp],#16
    ldp x23,x24,[sp],#16
    ldp x21,x22,[sp],#16
    ldp x19,x20,[sp],#16
    MSR(ELR,30)
    mov x9,#0x3c5
    MSR(SPSR,9)
    ERET

.global clock_ticks
clock_ticks:
    MRS(0,SYSREG(3,3,14,0,2)) // cntvct_el0
    ret
.global clock_hz
clock_hz:
    MRS(0,SYSREG(3,3,14,0,0)) // cntfrq_el0
    ret

.global smp_check_stop
smp_check_stop:
    b agent_test_expired
.global smp_abort_user
smp_abort_user:
    stp x9,x30,[sp,#-16]!
    b finish_user
.global swap_task
swap_task:
    stp x19,x20,[sp,#-16]!
    stp x21,x22,[sp,#-16]!
    stp x23,x24,[sp,#-16]!
    stp x25,x26,[sp,#-16]!
    stp x27,x28,[sp,#-16]!
    stp x29,x30,[sp,#-16]!
    stp d8,d9,[sp,#-16]!
    stp d10,d11,[sp,#-16]!
    stp d12,d13,[sp,#-16]!
    stp d14,d15,[sp,#-16]!
    MRS(9,ELR)
    MRS(10,SPSR)
    stp x9,x10,[sp,#-16]!
    MRS(9,USR_SP)
    mrs x10,fpcr
    mrs x11,fpsr
    stp x9,x10,[sp,#-16]!
    MRS(12,SYSREG(3,3,13,0,2)) // TPIDR_EL0
    stp x11,x12,[sp,#-16]!
    mov x9,sp
    str x9,[x0]
    .long 0xf100003f // cmp x1,#0
    bne resume_task
    mov sp,x2
    b task_start
resume_task:
    mov sp,x1
    ldp x11,x12,[sp],#16
    MSR(SYSREG(3,3,13,0,2),12)
    ldp x9,x10,[sp],#16
    msr fpsr,x11
    msr fpcr,x10
    MSR(USR_SP,9)
    ldp x9,x10,[sp],#16
    MSR(ELR,9)
    MSR(SPSR,10)
    ldp d14,d15,[sp],#16
    ldp d12,d13,[sp],#16
    ldp d10,d11,[sp],#16
    ldp d8,d9,[sp],#16
    ldp x29,x30,[sp],#16
    ldp x27,x28,[sp],#16
    ldp x25,x26,[sp],#16
    ldp x23,x24,[sp],#16
    ldp x21,x22,[sp],#16
    ldp x19,x20,[sp],#16
    ret
.global arm_timer
arm_timer:
    MSR(SYSREG(3,3,14,3,0),0) // cntv_tval_el0
    mov x0,#1
    MSR(SYSREG(3,3,14,3,1),0) // cntv_ctl_el0
    isb
    ret
.global gic3_enable
gic3_enable:
    MRS(0,SYSREG(3,0,12,12,5)) // ICC_SRE_EL1
    mov x1,#1
    orr x0,x0,x1
    MSR(SYSREG(3,0,12,12,5),0)
    isb
    mov x0,#255
    MSR(SYSREG(3,0,4,6,0),0) // ICC_PMR_EL1
    MSR(SYSREG(3,0,12,12,7),1) // ICC_IGRPEN1_EL1
    isb
    ret
.global gic3_ack
gic3_ack:
    MRS(0,SYSREG(3,0,12,12,0)) // ICC_IAR1_EL1
    ret
.global gic3_eoi
gic3_eoi:
    MSR(SYSREG(3,0,12,12,1),0) // ICC_EOIR1_EL1
    isb
    ret
