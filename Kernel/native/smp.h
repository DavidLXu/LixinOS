#ifndef LX_SMP_H
#define LX_SMP_H
/* Each CPU schedules its own kernel stacks. Shared kernel services use a fair
 * ticket lock, dropped before EL0, on an idle wait, or a cooperative yield. */
void smp_revoke_clock(unsigned cpu);
void smp_init(int gic3);
unsigned smp_cpu(void), smp_cpus(void);
unsigned long smp_clock_read(unsigned long mode), smp_busy_ticks(void);
unsigned long smp_idle_ticks(unsigned cpu);
void smp_idle_wait(unsigned long ticks), smp_kernel_yield(void);
#endif
