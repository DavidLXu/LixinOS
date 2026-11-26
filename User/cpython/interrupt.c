#include "../console.h"
#include "../tcc/lixin.h"
#include "runtime.h"
#include <Python.h>

void lx_signal_interrupt(void) { PyErr_SetInterrupt(); }

// Preserve ordinary typeahead when checking for a keyboard interrupt.
static unsigned char pending[4096];
static unsigned head, count;
int lx_check_interrupt(void) {
#ifdef LX_WAYLAND_INPUT
    // The compositor owns the physical keyboard. Polling stdin here would
    // take keystrokes away from the focused Wayland surface.
    return 0;
#else
    if (!lx_thread_main() || console_call(24, 2) != 1)
        return 0;
    while (count < sizeof(pending) && lx_poll(0, LX_READABLE, 0) == LX_READABLE) {
        int ch = lx_key_byte();
        if (ch == 3) {
            PyErr_SetInterrupt();
            return 1;
        }
        if ((unsigned)ch > 255)
            break;
        pending[(head + count++) % sizeof(pending)] = ch;
    }
    return 0;
#endif
}
int lx_input_byte(void) {
    if (count) {
        int c = pending[head];
        head = (head + 1) % sizeof(pending);
        count--;
        return c;
    }
    while (lx_threads_active() && lx_poll(0, LX_READABLE, 0) == 0)
        lx_delay(1);
    return lx_key_byte();
}
void lx_vm_tick(void) {
    static unsigned ticks;
    if (++ticks % 1024)
        return;
    if (lx_cooperative_active())
        lx_schedule();
    lx_check_interrupt();
}
