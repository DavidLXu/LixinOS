#include <lixin.h>
#include <stdio.h>

static void text(const char *s) {
    while (*s) putchar(*s++);
}

static int fail(const char *message) {
    lx_error(message);
    lx_error("\n");
    return 1;
}

static char *decimal(char *end, unsigned value) {
    do {
        *--end = '0' + value % 10;
        value /= 10;
    } while (value);
    return end;
}

static void row(const char *name, long size, int detail, int human, int color) {
    if (detail) {
        char buf[32];
        char *end = buf + sizeof buf - 1;
        char *p = end;
        *p = 0;
        putchar(size < 0 ? 'd' : '-');
        putchar(' ');
        if (size < 0) *--p = '-';
        else if (human && size >= 1024) {
            unsigned tenth = (size * 10 + 512) / 1024;
            *--p = 'B'; *--p = 'i'; *--p = 'K'; *--p = ' ';
            *--p = '0' + tenth % 10; *--p = '.';
            p = decimal(p, tenth / 10);
        } else {
            if (human) { *--p = 'B'; *--p = ' '; }
            p = decimal(p, size);
        }
        for (int n = end - p; n < 8; ++n) putchar(' ');
        text(p);
        text("  ");
    }
    int directory_color = color && size == LX_DIR;
    if (directory_color) text("\x1b[36m");
    text(name);
    if (directory_color) text("\x1b[39m");
    putchar('\n');
}

int main(int argc, char **argv) {
    int all = 0, detail = 1, human = 0, bytes = 0, options = 1;
    const char *operand = 0;
    const char *usage = "usage: ls [-1alh] [path]";
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (options && arg[0] == '-' && arg[1]) {
            if (arg[1] == '-' && !arg[2]) { options = 0; continue; }
            for (++arg; *arg; ++arg) {
                if (*arg == 'a') all = 1;
                else if (*arg == '1') detail = 0;
                else if (*arg == 'l') { detail = 1; bytes = 1; }
                else if (*arg == 'h') human = 1;
                else return fail(usage);
            }
        } else {
            if (operand) return fail(usage);
            operand = arg;
        }
    }
    human = human || !bytes;
    if (!operand) operand = ".";
    char name[513];
    long size = lx_stat(operand);
    if (size == -3) return fail("path not found");
    if (size == -4) return fail("not a directory");
    if (size < -2 || size == -1) return fail("invalid path");
    int color = lx_stdout_is_terminal();
    if (size >= 0) row(operand, size, detail, human, color);
    else {
        if (all) {
            row(".", LX_DIR, detail, human, color);
            row("..", LX_DIR, detail, human, color);
        }
        for (unsigned index = 0;; ++index) {
            size = lx_dir(operand, index, name);
            if (size == -1) break;
            if (size < -2) return fail("directory read failed");
            if (all || name[0] != '.') row(name, size, detail, human, color);
        }
    }
    return 0;
}
