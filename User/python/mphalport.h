#include "py/misc.h"

int lixin_readline(vstr_t *line, const char *prompt);
#define mp_hal_readline lixin_readline

static inline void mp_hal_set_interrupt_char(char c) { (void)c; }
static inline mp_uint_t mp_hal_ticks_ms(void) { return 0; }
