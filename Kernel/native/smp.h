#ifndef LX_SMP_H
#define LX_SMP_H
/* CPU 0 owns the kernel continuations. CPU 1 borrows a preempted EL0
 * exception frame until a primary-only syscall, fault or scheduling deadline. */
unsigned long smp_clock_read(unsigned long mode);
void smp_revoke_clock(void);
void smp_init(int gic3);
unsigned smp_cpus(void);
int smp_available(void);
int smp_submit(void *frame, unsigned long table);
int smp_finished(void);
unsigned long smp_collect(void);
unsigned long smp_busy_ticks(void);
unsigned long smp_current_root(void);
#endif
