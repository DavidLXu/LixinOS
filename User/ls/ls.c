#include <lixin.h>
#include <stdio.h>
#include "../file-time.h"

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

static void print_name(const char *name, long size, int color) {
    int directory_color = color && size == LX_DIR;
    if (directory_color) text("\x1b[36m");
    text(name);
    if (directory_color) text("\x1b[39m");
}

static void row(const char *name, long size, int detail, int human, int color, unsigned modified) {
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
        if (modified) {
            char date[24];
            format_utc(date, modified);
            text(date);
        } else {
            text("-                      ");
        }
        text("  ");
    }
    print_name(name, size, color);
    putchar('\n');
}

static unsigned name_length(const char *name) {
    unsigned length = 0;
    while (name[length]) ++length;
    return length;
}

struct columns {
    unsigned width, stride, used, padding;
};

static void column_name(struct columns *layout, const char *name, long size) {
    unsigned length = name_length(name);
    if (layout->used && layout->used + layout->padding + length > layout->width) {
        putchar('\n');
        layout->used = 0;
    }
    if (layout->used) {
        for (unsigned i = 0; i < layout->padding; ++i) putchar(' ');
        layout->used += layout->padding;
    }
    print_name(name, size, 1);
    layout->used += length;
    layout->padding = length < layout->stride ? layout->stride - length : 2;
}

int main(int argc, char **argv) {
    enum { COLUMNS, ONE_PER_LINE, LONG } format = COLUMNS;
    int all = 0, human = 0, options = 1;
    const char *operand = 0;
    const char *usage = "usage: ls [-1alh] [path]";
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (options && arg[0] == '-' && arg[1]) {
            if (arg[1] == '-' && !arg[2]) { options = 0; continue; }
            for (++arg; *arg; ++arg) {
                if (*arg == 'a') all = 1;
                else if (*arg == '1') format = ONE_PER_LINE;
                else if (*arg == 'l') format = LONG;
                else if (*arg == 'h') human = 1;
                else return fail(usage);
            }
        } else {
            if (operand) return fail(usage);
            operand = arg;
        }
    }
    if (!operand) operand = ".";
    int detail = format == LONG;
    char name[513];
    long size = lx_stat(operand);
    if (size == -3) return fail("path not found");
    if (size == -4) return fail("not a directory");
    if (size < -2 || size == -1) return fail("invalid path");
    int color = lx_stdout_is_terminal();
    if (size >= 0) row(operand, size, detail, human, color, lx_file_time(1));
    else {
        struct columns layout = {0, 0, 0, 0};
        if (color && format == COLUMNS) {
            layout.width = lx_geometry() & 65535;
            if (!layout.width) layout.width = 80;
            unsigned longest = all ? 2 : 0;
            for (unsigned index = 0;; ++index) {
                size = lx_dir(operand, index, name);
                if (size == LX_END) break;
                if (size < LX_DIR) return fail("directory read failed");
                if (all || name[0] != '.') {
                    unsigned length = name_length(name);
                    if (length > longest) longest = length;
                }
            }
            layout.stride = longest + 2;
            if (all) {
                column_name(&layout, ".", LX_DIR);
                column_name(&layout, "..", LX_DIR);
            }
        } else if (all) {
            row(".", LX_DIR, detail, human, color, lx_file_time(1));
            unsigned i = 0;
            while (operand[i] && i < 509) { name[i] = operand[i]; ++i; }
            unsigned parent_time = 0;
            if (!operand[i]) {
                name[i++] = '/'; name[i++] = '.'; name[i++] = '.'; name[i] = 0;
                if (lx_stat(name) == LX_DIR) parent_time = lx_file_time(1);
            }
            row("..", LX_DIR, detail, human, color, parent_time);
        }
        for (unsigned index = 0;; ++index) {
            size = lx_dir(operand, index, name);
            if (size == LX_END) break;
            if (size < LX_DIR) return fail("directory read failed");
            if (!all && name[0] == '.') continue;
            if (layout.width) column_name(&layout, name, size);
            else row(name, size, detail, human, color, lx_file_time(1));
        }
        if (layout.used) putchar('\n');
    }
    return 0;
}
