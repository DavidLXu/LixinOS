#include "smp.h"
typedef unsigned long word;
#define REG(addr) (*(volatile unsigned *)(unsigned long)(addr))
extern long smp_power_on(word *boot);
extern void smp_gic3_enable(void);
extern word smp_ticks(void), smp_frequency(void);
extern void smp_timer(word ticks);

static word boot[7];
static int use_gic3;
extern word smp_acquire(const volatile word *), smp_take_ticket(volatile word *);
extern void smp_release(volatile word *, word);
extern void smp_bind_kernel(void), smp_secondary_schedule(void);
static volatile word online, fast_clock[2];
extern int smp_clock_allowed(void);
void smp_revoke_clock(unsigned cpu) { smp_release(&fast_clock[cpu], 0); }
static volatile word next_ticket = 1; /* CPU 0 starts in EL1 holding ticket 0. */
volatile word smp_serving;
static word idle[2], idle_since[2], secondary_start;
static void notify(void) { __asm__ volatile("dsb ish\nsev" ::: "memory"); }

void smp_kernel_enter(void) {
    word ticket = smp_take_ticket(&next_ticket);
    while (smp_acquire(&smp_serving) != ticket) __asm__ volatile("wfe" ::: "memory");
    smp_revoke_clock(smp_cpu());
    smp_bind_kernel();
}
static void unlock(void) {
    smp_release(&smp_serving, smp_serving + 1);
    notify();
}
void smp_kernel_yield(void) {
    if (smp_cpus() == 1) return;
    unlock();
    smp_kernel_enter();
}
void smp_idle_wait(word delay) {
    unsigned c = smp_cpu();
    idle_since[c] = smp_ticks();
    smp_timer(delay ? delay : 1);
    unlock();
    __asm__ volatile("wfi" ::: "memory");
    smp_kernel_enter();
    idle[c] += smp_ticks() - idle_since[c];
    idle_since[c] = 0;
}
word smp_idle_ticks(unsigned c) {
    return idle[c] + (idle_since[c] ? smp_ticks() - idle_since[c] : 0);
}
word smp_busy_ticks(void) {
    if (!secondary_start) return 0;
    word idle_time = smp_idle_ticks(1), now = smp_ticks();
    return now - secondary_start - idle_time;
}

static word clock_value(word mode) {
    word ticks = smp_ticks();
    if (mode == 2) return ticks;
    if (mode > 2) return ~0UL;
    word frequency = smp_frequency(), scale = mode ? 1000000 : 1000;
    return (ticks / frequency) * scale + (ticks % frequency) * scale / frequency;
}
word smp_clock_read(word mode) {
    if (smp_cpu() == 1 && smp_clock_allowed()) smp_release(&fast_clock[1], 1);
    return clock_value(mode);
}
int smp_enter_svc(word *saved_x0, word operation, word esr) {
    if ((esr >> 26) == 0x15 && operation == 38 && smp_acquire(&fast_clock[smp_cpu()])) {
        *saved_x0 = clock_value(*saved_x0);
        return 1;
    }
    smp_kernel_enter();
    return 0;
}

void smp_init(int gic3) {
    use_gic3 = gic3;
    if (smp_power_on(boot)) return; /* PSCI rejects CPU 1 on a one-vCPU machine. */
    word deadline = smp_ticks() + smp_frequency();
    while (!smp_acquire(&online) && smp_ticks() < deadline) {}
}
unsigned smp_cpus(void) { return smp_acquire(&online) ? 2 : 1; }
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
    smp_kernel_enter();
    secondary_start = smp_ticks();
    smp_timer(smp_frequency() / 50);
    smp_secondary_schedule();
    for (;;) __asm__ volatile("wfe");
}
