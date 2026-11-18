// Disk-resident text utilities. The build selects one command per ELF.
#include "../console.h"
#include "../tcc/lixin.h"
#ifndef COMMAND
#error Select a command with -DCOMMAND
#endif
int lx_getchar(void);
static unsigned length(const char *s) {
    unsigned n = 0;
    while (s[n])
        ++n;
    return n;
}
static int equal(const char *a, const char *b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}
static void output(const char *s, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        put_byte(s[i]);
}
static int error(const char *s) {
    lx_error(s);
    lx_error("\n");
    return 1;
}
static int cat(const char *path) {
    if (!path) {
        int c;
        while ((c = lx_getchar()) >= 0)
            put_byte(c);
        return 0;
    }
    int fd = lx_open(path);
    if (fd < 0)
        return error(lx_stat(path) == LX_DIR ? "is a directory" : "file not found");
    char buf[512];
    int n;
    while ((n = lx_read(fd, buf, sizeof(buf))) > 0)
        output(buf, n);
    lx_close(fd);
    return n < 0 ? error("file read failed") : 0;
}
static int grep(const char *pattern) {
    // This utility has its own private 2 MiB writable segment.
    char *line = (char *)0x44200000;
    unsigned n = 0, patlen = length(pattern);
    for (;;) {
        int c = lx_getchar();
        if (c < 0 && !n)
            break;
        if (c >= 0 && c != '\n') {
            if (n == 1024 * 1024)
                return error("input line too long");
            line[n++] = c;
            continue;
        }
        if (c == '\n' && n && line[n - 1] == '\r')
            --n;
        for (unsigned i = 0; i + patlen <= n; i++) {
            unsigned j = 0;
            while (j < patlen && line[i + j] == pattern[j])
                ++j;
            if (j == patlen) {
                output(line, n);
                put_byte('\n');
                break;
            }
        }
        n = 0;
        if (c < 0)
            break;
    }
    return 0;
}
__attribute__((section(".text.start"), noreturn)) void _start(const char *cwd, const char *args,
                                                              unsigned long len) {
    const char *end = args + len;
    if (equal(COMMAND, "echo")) {
        for (const char *p = args; p < end;) {
            unsigned n = length(p);
            output(p, n);
            p += n + 1;
            if (p < end)
                put_byte(' ');
        }
        put_byte('\n');
        finish(0);
    }
    if (equal(COMMAND, "clear")) {
        if (len)
            finish(1);
        output("\033[2J\033[H", 7);
        finish(0);
    }
    if (equal(COMMAND, "netinfo")) {
        if (len) finish(error("usage: netinfo"));
        long n = console_call(29, 0);
        if (n < 0) finish(error("eth0 unavailable"));
        output((const char *)0x443d7000, n);
        finish(0);
    }
    if (equal(COMMAND, "pwd")) {
        if (len)
            finish(error("pwd takes no arguments"));
        output(cwd, length(cwd));
        put_byte('\n');
        finish(0);
    }
    if (equal(COMMAND, "cat") && !len)
        finish(cat(0));
    if (!len || args + length(args) + 1 != end)
        finish(error("expected one argument"));
    if (equal(COMMAND, "cat"))
        finish(cat(args));
    if (equal(COMMAND, "grep"))
        finish(grep(args));
    if (!equal(args, "-l"))
        finish(error("usage: wc -l"));
    unsigned count = 0;
    int c;
    while ((c = lx_getchar()) >= 0)
        if (c == '\n')
            ++count;
    char digits[12];
    unsigned i = sizeof(digits);
    do {
        digits[--i] = '0' + count % 10;
        count /= 10;
    } while (count);
    output(digits + i, sizeof(digits) - i);
    put_byte('\n');
    finish(0);
}
