#include "../tcc/lixin.h"
#include "../console.h"
#include "../cpython/runtime.h"

void lx_signal_interrupt(void) { finish(130); }
int lx_check_interrupt(void) { return 0; }
int lx_input_byte(void) {
    while (lx_threads_active() && lx_poll(0,LX_READABLE,0)==0) lx_delay(1);
    return lx_key_byte();
}
