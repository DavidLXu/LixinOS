// Entry point for the preinstalled raw cecho image.
#include "../console.h"

int main(void);
int getchar(void) { return get_byte(); }
int putchar(int c) {
    if (c == '\n') put_byte('\r');
    put_byte(c);
    return (unsigned char)c;
}

__attribute__((section(".text.start"), noreturn))
void _start(void) { finish(main()); }
