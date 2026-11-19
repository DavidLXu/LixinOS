#include "../tcc/lixin.h"
#include "../console.h"
#include "../cpython/runtime.h"

void lx_signal_interrupt(void) { finish(130); }
int lx_check_interrupt(void) { return 0; }
int lx_input_byte(void) { return lx_key_byte(); }
