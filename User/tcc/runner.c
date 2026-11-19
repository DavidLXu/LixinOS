// Shared runtime for version 1 LixinOS executables. Each ABI slot is one
// fixed-width ARM64 branch; newlib itself may move between runtime builds.
#include "../console.h"
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include "lixin.h"

// Operations on the process-service mailbox (User/sh/abi.rs).
enum { SPAWN_BEGIN = 4, SPAWN_ARG = 5, CHILD_STATUS = 7, PANE_READY = 15 };
static jmp_buf waiting;
static long process_arg(unsigned long op, const char *arg) {
    char *mail = (char *)0x443d8000;
    size_t n = strlen(arg);
    if (n > 4096) return -1;
    memcpy(mail, arg, n + 1);
    return console_call(18, op);
}
int lx_spawn_flags(const char *path, int argc, char **argv, unsigned flags) {
    if (!path || argc < 1 || argc > 512 || !argv || process_arg(((unsigned long)flags << 32) | SPAWN_BEGIN, path) < 0) return -1;
    for (int i = 0; i < argc; ++i)
        if (!argv[i] || process_arg(SPAWN_ARG, argv[i]) < 0) return -1;
    if (setjmp(waiting) == 0) {
        console_call(32, 0);
        __builtin_unreachable();
    }
    return process_arg(CHILD_STATUS, "");
}
int lx_spawn(const char *path, int argc, char **argv) {
    return lx_spawn_flags(path, argc, argv, 0);
}
int lx_agent_yield(void) {
    if (setjmp(waiting) == 0) {
        console_call(32, 0);
        __builtin_unreachable();
    }
    return 0;
}
static unsigned startup_flags;
unsigned lx_start_flags(void) { return startup_flags; }
asm(".section .lxstdio,\"ax\"\n"
#define BRANCH_TO(symbol) "b " #symbol "\n"
#define SLOT(name, impl) BRANCH_TO(impl)
#define LIBC(name) BRANCH_TO(name)
#include "slots.def"
#undef LIBC
#undef SLOT
#undef BRANCH_TO
    ".previous");
__attribute__((section(".text.start"), noreturn))
void _start(int (*main_fn)(int, char **), int argc, char *packed, unsigned flags) {
    if (!main_fn) longjmp(waiting, 1);
    char *argv[argc + 1];
    for (int i = 0; i < argc; ++i) {
        // Process requests reuse the argument mailbox.
        argv[i] = strdup(packed);
        if (!argv[i]) finish(1);
        while (*packed++) {}
    }
    argv[argc] = NULL;
    startup_flags = flags;
    setbuf(stdout, NULL);
    finish((unsigned char)main_fn(argc, argv));
}
