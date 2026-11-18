#include "../console.h"
#include <setjmp.h>
#include <stddef.h>
#include <errno.h>
extern char __user_heap_start[], __user_heap_limit[];
void *_sbrk(ptrdiff_t n) {
    static char *p = __user_heap_start;
    if (n < 0 || n > __user_heap_limit - p) { errno = ENOMEM; return (void *)-1; }
    char *old = p; p += n; return old;
}
void _exit(int code) { finish(code); }
int read_raw(void) { return get_byte(); }
int read_timed(unsigned ms) { return console_call(14, ms); }
unsigned long screen_info(void) { return console_call(13, 0); }
void screen_write(const char *p, size_t n) { while(n--) put_byte(*p++); }
int process_call(unsigned op, unsigned index) { return console_call(18, ((unsigned long)index<<32)|op); }

// The synchronous spawn ABI suspends the caller. On re-entry its writable
// segment and stack have been restored, so newlib can restore the continuation.
static jmp_buf continuation;
void wait_child(void) {
    if (setjmp(continuation) == 0) {
        console_call(32, 0);
        __builtin_unreachable();
    }
}
extern int sh_main(const char *, unsigned);
__attribute__((section(".text.start"),noreturn))
void _start(const char *cwd, unsigned flags) {
    if (!cwd) longjmp(continuation, 1);
    finish(sh_main(cwd, flags));
}
