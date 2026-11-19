#include "py/misc.h"

int lixin_readline(vstr_t *line, const char *prompt);
#define mp_hal_readline lixin_readline

static inline void mp_hal_set_interrupt_char(char c) { (void)c; }
mp_uint_t mp_hal_ticks_ms(void);
mp_uint_t mp_hal_ticks_us(void);
mp_uint_t mp_hal_ticks_cpu(void);
uint64_t mp_hal_time_ns(void);
void mp_hal_delay_ms(mp_uint_t ms);
void mp_hal_delay_us(mp_uint_t us);
