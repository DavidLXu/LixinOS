#include "../console.h"
int main(int argc, char **argv);
int putchar(int c) { put_byte(c); return (unsigned char)c; }
__attribute__((section(".text.start"), noreturn))
void _start(const char *cwd, char *args, unsigned long len) {
    (void)cwd;
    char *argv[258];
    argv[0] = "ls";
    int argc = 1;
    for (char *p = args; p < args + len;) {
        if (argc == 257) finish(1);
        argv[argc++] = p;
        while (*p++) {}
    }
    argv[argc] = 0;
    finish(main(argc, argv));
}
