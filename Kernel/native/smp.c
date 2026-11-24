#include "smp.h"
typedef unsigned long word;
#define REG(addr) (*(volatile unsigned *)(unsigned long)(addr))
extern long smp_power_on(word *boot);
extern void smp_resume(void *frame, word root);
extern void smp_root(word root);
extern void smp_gic3_enable(void), smp_gic3_eoi(word);
extern word smp_gic3_ack(void), smp_ticks(void), smp_frequency(void);
extern void smp_timer(word ticks), smp_timer_off(void);

static word boot[6];
static int use_gic3;
extern word smp_acquire(const volatile word *);
extern void smp_release(volatile word *, word);
static volatile word online, state;
enum { FREE, DISPATCHED, RETURNED };
static void *user_frame;
static word user_root;
static volatile word local_clock;
extern int smp_clock_allowed(void);
extern void smp_continue(void *frame) __attribute__((noreturn));

/* smp_save pushes the general registers in pairs from x0 upwards. */
enum { FRAME_ELR = 0, FRAME_X8 = (816 - 5 * 16) / 8, FRAME_X0 = (816 - 16) / 8 };

word smp_clock_read(word mode) {
    word ticks = smp_ticks();
    if (mode == 2) return ticks;
    if (mode > 2) return ~0UL;
    word frequency = smp_frequency(), scale = mode ? 1000000 : 1000;
    return (ticks / frequency) * scale + (ticks % frequency) * scale / frequency;
}
void smp_revoke_clock(void) { smp_release(&local_clock, 0); }
/* Single secondary writer. Readers can sample an in-flight quantum. */
static volatile word sample_seq, busy, started;
static word elapsed;

static void barrier(void) { __asm__ volatile("dmb ish" ::: "memory"); }
static void notify(void) { __asm__ volatile("dsb ish\nsev" ::: "memory"); }

void smp_init(int gic3) {
    use_gic3 = gic3;
    if (smp_power_on(boot)) return; /* PSCI rejects CPU 1 on a one-vCPU machine. */
    word deadline = smp_ticks() + smp_frequency();
    while (!smp_acquire(&online) && smp_ticks() < deadline) {}
    barrier();
}
unsigned smp_cpus(void) { return smp_acquire(&online) ? 2 : 1; }
int smp_available(void) { return smp_acquire(&online) && smp_acquire(&state) == FREE; }
int smp_submit(void *frame, word table) {
    if (!table || !smp_available()) return 0;
    user_frame = frame;
    user_root = table;
    smp_release(&local_clock, smp_clock_allowed());
    smp_release(&state, DISPATCHED);
    notify();
    return 1;
}
int smp_finished(void) { return smp_acquire(&state) == RETURNED; }
word smp_collect(void) {
    smp_release(&state, FREE);
    return elapsed;
}
word smp_busy_ticks(void) {
    for (;;) {
        word seq = smp_acquire(&sample_seq);
        if (seq & 1) continue;
        word total = busy, begin = started;
        if (begin) total += smp_ticks() - begin;
        barrier();
        if (seq == smp_acquire(&sample_seq)) return total;
    }
}
static void dispatch(void) {
    while (smp_acquire(&state) != DISPATCHED) __asm__ volatile("wfe" ::: "memory");
    sample_seq++;
    barrier();
    started = smp_ticks();
    smp_release(&sample_seq, sample_seq + 1);
    smp_timer(smp_frequency() / 10);
    smp_resume(user_frame, user_root);
}
void smp_secondary_main(void) {
    if (use_gic3) {
        /* QEMU virt places redistributor 1 two 64 KiB pages after CPU 0. */
        REG(0x080c0014) &= ~2u;
        while (REG(0x080c0014) & 4) {}
        REG(0x080d0080) |= 1u << 27;
        *(volatile unsigned char *)0x080d041b = 0x80;
        REG(0x080d0100) = 1u << 27;
        smp_gic3_enable();
    } else {
        REG(0x08000100) = 1u << 27;
        *(volatile unsigned char *)0x0800041b = 0x80;
        REG(0x08010004) = 255;
        REG(0x08010000) = 1;
    }
    smp_release(&online, 1);
    notify();
    dispatch();
}
void smp_secondary_trap(word kind, word esr) {
    word *frame = user_frame;
    if (kind == 1 && (esr >> 26) == 0x15 && frame[FRAME_X8] == 38 && smp_acquire(&local_clock)) {
        frame[FRAME_X0] = smp_clock_read(frame[FRAME_X0]);
        /* Keep the original timer deadline, page table and frame ownership. */
        smp_continue(frame);
    }
    smp_timer_off();
    if (kind == 2) {
        word id = use_gic3 ? smp_gic3_ack() : REG(0x0801000c);
        if ((id & 1023) < 1020) {
            if (use_gic3) smp_gic3_eoi(id);
            else REG(0x08010010) = id;
        }
    } else if ((esr >> 26) == 0x15) {
        /* Re-execute the SVC on CPU 0, in the normal syscall and fault path. */
        frame[FRAME_ELR] -= 4;
    }
    sample_seq++;
    barrier();
    elapsed = smp_ticks() - started;
    busy += elapsed;
    started = 0;
    smp_release(&sample_seq, sample_seq + 1);
    /* Relinquish even the table-walk reference before the owner may exit. */
    smp_root(boot[0]);
    smp_release(&state, RETURNED);
    notify();
    dispatch();
}
