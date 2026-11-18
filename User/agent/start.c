#include "../console.h"
#include <setjmp.h>

int main(void);
// This ELF has just an RX segment. Keep the saved registers in its private
// utility page, separate from the model request and reply mailbox.
#define WAITING (*(jmp_buf *)0x443d7000UL)

int lx_agent_yield(void) {
    if (setjmp(WAITING) == 0) {
        console_call(32, 0);
        __builtin_unreachable();
    }
    return 0;
}

__attribute__((section(".text.start"), noreturn))
void _start(void *unused, void *args, unsigned long len, unsigned long flags) {
    (void)unused; (void)args; (void)len;
    if (flags == 65536) longjmp(WAITING, 1);
    finish(main());
}
