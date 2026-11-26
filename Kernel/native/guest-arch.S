// Native AArch64 EL0 guest entry, trap exit, cache and address translation helpers.
// Uses only encodings accepted by both GNU as and TinyCC's built-in A64 assembler.
#define REG(a,b,c,d,e) (((a)<<14)|((b)<<11)|((c)<<7)|((d)<<3)|(e))
#define RD(x,r) .long (0xd5300000|((r)<<5)|(x))
#define WR(r,x) .long (0xd5100000|((r)<<5)|(x))
#define ELR    REG(3,0,4,0,1)
#define SPSR   REG(3,0,4,0,0)
#define USP    REG(3,0,4,1,0)
#define TLS    REG(3,3,13,0,2)
#define ESR    REG(3,0,5,2,0)
#define FAR    REG(3,0,6,0,0)
#define PAR    REG(3,0,7,4,0)
#define MPIDR  REG(3,0,0,0,5)
#define CTR    REG(3,3,0,0,1)
// Signed-offset 128-bit SIMD pair load/store relative to x17 (idx = offset/16).
#define STPQ(a,b,idx) .long (0xad000000 | (((idx) & 0x7f) << 15) | ((b) << 10) | (17 << 5) | (a))
#define LDPQ(a,b,idx) .long (0xad400000 | (((idx) & 0x7f) << 15) | ((b) << 10) | (17 << 5) | (a))
#define PUSHQ(a,b) .long (0xadbf0000 | ((b) << 10) | (31 << 5) | (a))
#define POPQ(a,b)  .long (0xacc10000 | ((b) << 10) | (31 << 5) | (a))

.text

// Fast check called from user_sync (caller pushed x0,x1 and x2,x30).
// Returns active guest shadow ctx pointer in x0 (or 0). Clobbers only x0, x1, x2.
.global guest_current_ctx
guest_current_ctx:
    RD(0, MPIDR)
    and x0, x0, #1
    mov x2, x30
    bl guest_running_ptr
    .quad guest_running
guest_running_ptr:
    ldr x1, [x30]
    mov x30, x2
    lsl x0, x0, #3
    add x1, x1, x0
    ldr x0, [x1]
    ret

// Enter native guest EL0 with registers from shadow ctx (x0).
// Returns exit_reason (1..5) once the guest traps back to host.
.global guest_enter
guest_enter:
    stp x19, x20, [sp, #-16]!
    stp x21, x22, [sp, #-16]!
    stp x23, x24, [sp, #-16]!
    stp x25, x26, [sp, #-16]!
    stp x27, x28, [sp, #-16]!
    stp x29, x30, [sp, #-16]!
    stp d8,  d9,  [sp, #-16]!
    stp d10, d11, [sp, #-16]!
    stp d12, d13, [sp, #-16]!
    stp d14, d15, [sp, #-16]!
    RD(9, ELR)
    RD(10, SPSR)
    stp x9, x10, [sp, #-16]!
    RD(9, USP)
    RD(10, TLS)
    stp x9, x10, [sp, #-16]!
    mrs x9, fpcr
    mrs x10, fpsr
    stp x9, x10, [sp, #-16]!
    stp x0, xzr, [sp, #-16]!   // [sp+0] = shadow pointer
    mov x19, x0
    mov x0, sp
    bl guest_record_enter      // save saved_sp, mark active, publish guest_running[cpu]

    add x17, x19, #320
    LDPQ(0,  1,  0)
    LDPQ(2,  3,  2)
    LDPQ(4,  5,  4)
    LDPQ(6,  7,  6)
    LDPQ(8,  9,  8)
    LDPQ(10, 11, 10)
    LDPQ(12, 13, 12)
    LDPQ(14, 15, 14)
    LDPQ(16, 17, 16)
    LDPQ(18, 19, 18)
    LDPQ(20, 21, 20)
    LDPQ(22, 23, 22)
    LDPQ(24, 25, 24)
    LDPQ(26, 27, 26)
    LDPQ(28, 29, 28)
    LDPQ(30, 31, 30)

    ldp x9, x10, [x19, #280]   // fpcr, fpsr
    msr fpcr, x9
    msr fpsr, x10
    ldr x9, [x19, #272]        // tpidr
    WR(TLS, 9)
    ldr x9, [x19, #248]        // sp
    WR(USP, 9)
    ldp x9, x10, [x19, #256]   // pc, pstate
    WR(ELR, 9)
    lsr x10, x10, #28
    and x10, x10, #15
    lsl x10, x10, #28          // EL0t with DAIF=0, NZCV preserved
    WR(SPSR, 10)

    ldp x0,  x1,  [x19, #0]
    ldp x2,  x3,  [x19, #16]
    ldp x4,  x5,  [x19, #32]
    ldp x6,  x7,  [x19, #48]
    ldp x8,  x9,  [x19, #64]
    ldp x10, x11, [x19, #80]
    ldp x12, x13, [x19, #96]
    ldp x14, x15, [x19, #112]
    ldp x16, x17, [x19, #128]
    ldr x18,      [x19, #144]
    ldp x20, x21, [x19, #160]
    ldp x22, x23, [x19, #176]
    ldp x24, x25, [x19, #192]
    ldp x26, x27, [x19, #208]
    ldp x28, x29, [x19, #224]
    ldr x30,      [x19, #240]
    ldr x19,      [x19, #152]
    b smp_leave_user

// Synchronous exception from active guest EL0.
// On entry: x0 = shadow ctx, [sp+0] = guest (x2, x30), [sp+16] = guest (x0, x1).
.global guest_trap_sync
guest_trap_sync:
    RD(1, ESR)
    lsr x1, x1, #26
    sub x2, x1, #0x20
    cbz x2, guest_try_fault
    sub x2, x1, #0x24
    cbz x2, guest_try_fault
    b guest_save_and_exit

guest_try_fault:
    stp x3,  x4,  [sp, #-16]!
    stp x5,  x6,  [sp, #-16]!
    stp x7,  x8,  [sp, #-16]!
    stp x9,  x10, [sp, #-16]!
    stp x11, x12, [sp, #-16]!
    stp x13, x14, [sp, #-16]!
    stp x15, x16, [sp, #-16]!
    stp x17, x18, [sp, #-16]!
    stp x29, xzr, [sp, #-16]!
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
    RD(0, ESR)
    RD(1, FAR)
    bl guest_handle_fault
    cbz x0, guest_fault_unhandled
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
    ldp x29, xzr, [sp], #16
    ldp x17, x18, [sp], #16
    ldp x15, x16, [sp], #16
    ldp x13, x14, [sp], #16
    ldp x11, x12, [sp], #16
    ldp x9,  x10, [sp], #16
    ldp x7,  x8,  [sp], #16
    ldp x5,  x6,  [sp], #16
    ldp x3,  x4,  [sp], #16
    ldp x2,  x30, [sp], #16
    ldp x0,  x1,  [sp], #16
    b smp_leave_user

guest_fault_unhandled:
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
    ldp x29, xzr, [sp], #16
    ldp x17, x18, [sp], #16
    ldp x15, x16, [sp], #16
    ldp x13, x14, [sp], #16
    ldp x11, x12, [sp], #16
    ldp x9,  x10, [sp], #16
    ldp x7,  x8,  [sp], #16
    ldp x5,  x6,  [sp], #16
    ldp x3,  x4,  [sp], #16
    bl guest_current_ctx

guest_save_and_exit:
    // x0 = shadow ctx; [sp+0] = guest (x2, x30); [sp+16] = guest (x0, x1).
    str x3,       [x0, #24]
    stp x4,  x5,  [x0, #32]
    stp x6,  x7,  [x0, #48]
    stp x8,  x9,  [x0, #64]
    stp x10, x11, [x0, #80]
    stp x12, x13, [x0, #96]
    stp x14, x15, [x0, #112]
    stp x16, x17, [x0, #128]
    stp x18, x19, [x0, #144]
    stp x20, x21, [x0, #160]
    stp x22, x23, [x0, #176]
    stp x24, x25, [x0, #192]
    stp x26, x27, [x0, #208]
    stp x28, x29, [x0, #224]
    ldp x9,  x10, [sp], #16    // guest x2, x30
    str x9,       [x0, #16]
    str x10,      [x0, #240]
    ldp x9,  x10, [sp], #16    // guest x0, x1
    stp x9,  x10, [x0, #0]

    RD(9, USP)
    str x9, [x0, #248]
    RD(9, ELR)
    RD(10, SPSR)
    stp x9, x10, [x0, #256]
    RD(9, TLS)
    mrs x10, fpcr
    stp x9, x10, [x0, #272]
    mrs x9, fpsr
    str x9, [x0, #288]

    RD(9, ESR)
    RD(10, FAR)
    stp x9, x10, [x0, #304]
    lsr x11, x9, #26
    mov x12, #1                // LX_GUEST_EXIT_SVC
    sub x13, x11, #0x15
    cbz x13, guest_store_reason
    mov x12, #2                // LX_GUEST_EXIT_FAULT
    sub x13, x11, #0x20
    cbz x13, guest_store_reason
    sub x13, x11, #0x24
    cbz x13, guest_store_reason
    mov x12, #3                // LX_GUEST_EXIT_SYNC
guest_store_reason:
    str x12, [x0, #296]

    add x17, x0, #320
    STPQ(0,  1,  0)
    STPQ(2,  3,  2)
    STPQ(4,  5,  4)
    STPQ(6,  7,  6)
    STPQ(8,  9,  8)
    STPQ(10, 11, 10)
    STPQ(12, 13, 12)
    STPQ(14, 15, 14)
    STPQ(16, 17, 16)
    STPQ(18, 19, 18)
    STPQ(20, 21, 20)
    STPQ(22, 23, 22)
    STPQ(24, 25, 24)
    STPQ(26, 27, 26)
    STPQ(28, 29, 28)
    STPQ(30, 31, 30)

    bl guest_finish_exit       // returns saved_sp in x0

.global guest_unwind_to_host
guest_unwind_to_host:
    mov sp, x0
    ldp x16, xzr, [sp], #16    // shadow ctx
    ldp x9, x10, [sp], #16
    msr fpcr, x9
    msr fpsr, x10
    ldp x9, x10, [sp], #16
    WR(USP, 9)
    WR(TLS, 10)
    ldp x9, x10, [sp], #16
    WR(ELR, 9)
    WR(SPSR, 10)
    ldp d14, d15, [sp], #16
    ldp d12, d13, [sp], #16
    ldp d10, d11, [sp], #16
    ldp d8,  d9,  [sp], #16
    ldp x29, x30, [sp], #16
    ldp x27, x28, [sp], #16
    ldp x25, x26, [sp], #16
    ldp x23, x24, [sp], #16
    ldp x21, x22, [sp], #16
    ldp x19, x20, [sp], #16
    ldr x0, [x16, #296]        // exit_reason
    ret

// Translate EL0 VA (x0) with read (x1=0) or write (x1=1) check.
// Returns 4 KiB-aligned PA, or 0 on translation/permission failure.
.global guest_va_to_pa
guest_va_to_pa:
    cbz x1, guest_va_read
    .long 0xd5087860           // at s1e0w, x0
    b guest_va_done
guest_va_read:
    .long 0xd5087840           // at s1e0r, x0
guest_va_done:
    isb
    RD(0, PAR)
    and x1, x0, #1
    cbnz x1, guest_va_fail
    movz x1, #0xf000
    movk x1, #0xffff, lsl #16
    movk x1, #0xffff, lsl #32
    and x0, x0, x1
    ret
guest_va_fail:
    mov x0, #0
    ret

// Single-VA inner-shareable EL1/EL0 TLB invalidation.
.global guest_tlbi_va
guest_tlbi_va:
    dsb ishst
    lsr x0, x0, #12
    .long 0xd5088360           // tlbi vaae1is, x0
    dsb ish
    isb
    ret

// Clean D-cache lines by VA to PoU using CTR_EL0.DminLine.
// Aligns both start and end to cache line boundaries so loop exits on exact equality.
.global guest_clean_dcache
guest_clean_dcache:
    cbz x1, guest_clean_done
    RD(2, CTR)
    lsr x2, x2, #16
    and x2, x2, #15
    mov x3, #4
    lsl x3, x3, x2             // line bytes (power of 2)
    sub x4, x3, #1             // line mask
    add x1, x0, x1             // end
    add x1, x1, x4
    and x5, x1, x4
    sub x1, x1, x5             // end aligned up
    and x5, x0, x4
    sub x0, x0, x5             // start aligned down
guest_clean_loop:
    .long 0xd50b7b20           // dc cvau, x0
    add x0, x0, x3
    sub x5, x0, x1
    cbnz x5, guest_clean_loop
    dsb ish
guest_clean_done:
    ret

.global guest_icache_sync
guest_icache_sync:
    dsb ish
    .long 0xd508711f           // ic ialluis
    dsb ish
    isb
    ret
