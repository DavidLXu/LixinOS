#pragma once
#include <stddef.h>
#include <stdint.h>
#include <time.h>
int lx_thread_id(void);
int lx_thread_main(void);
int lx_threads_active(void);
long lx_clear_tid(int *);
void lx_schedule(void);
void lx_delay(unsigned);
long lx_sigmask(int, const uint64_t *, uint64_t *);
long lx_futex(int *, int, int, const struct timespec *, int *, int);
_Noreturn void lx_thread_exit(void *, size_t);
int lx_check_interrupt(void);
int lx_input_byte(void);
void lx_signal_interrupt(void);
