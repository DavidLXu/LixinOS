// Shared runtime for version 1 LixinOS executables. Each ABI slot is one
// fixed-width ARM64 branch; newlib itself may move between runtime builds.
#include "../console.h"
#include <stdio.h>
asm(".section .lxstdio,\"ax\"\n"
    "b puts\nb printf\nb putchar\n.previous");
__attribute__((section(".text.start"), noreturn))
void _start(int (*main_fn)(void)) {
    setbuf(stdout, NULL);
    int status = main_fn();
    printf("[exit %d]\n", status);
    finish(0);
}
