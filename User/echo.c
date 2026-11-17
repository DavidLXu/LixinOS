#include "console.h"
__attribute__((section(".text.start"), noreturn)) void _start(void) {
    const char *message = "C echo (one line)> ";
    while (*message) put_byte(*message++);
    int c;
    while ((c = get_byte()) != '\r' && c != '\n' && c != 4) put_byte(c);
    put_byte('\r');
    put_byte('\n');
    finish(0);
}
