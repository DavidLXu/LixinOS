// Shared stream readers for the small text utilities.
typedef struct {
    int fd, at, size;
    char bytes[1024];
} Input;

static int input_open(Input *in, const char *path) {
    in->at = in->size = 0;
    in->fd = path && !equal(path, "-") ? lx_open(path) : 0;
    return in->fd >= 0;
}

static int input_byte(Input *in) {
    if (!in->fd) return lx_getchar();
    if (in->at == in->size && in->size >= 0) {
        in->size = lx_read(in->fd, in->bytes, sizeof(in->bytes));
        in->at = 0;
    }
    if (in->size <= 0) return -1;
    return (unsigned char)in->bytes[in->at++];
}

static void input_close(Input *in) { if (in->fd > 0) lx_close(in->fd); }

static void number(unsigned long value) {
    char digits[24];
    unsigned i = sizeof(digits);
    do {
        digits[--i] = '0' + value % 10;
        value /= 10;
    } while (value);
    output(digits + i, sizeof(digits) - i);
}

static int count_arg(const char *s, unsigned *value) {
    unsigned n = 0;
    if (!*s) return 0;
    while (*s >= '0' && *s <= '9') {
        if (n > 100000000) return 0;
        n = n * 10 + (*s++ - '0');
    }
    if (*s) return 0;
    *value = n;
    return 1;
}

static int head_tail(int argc, char **argv, int tail) {
    unsigned lines = 10;
    int at = 0;
    if (at < argc && equal(argv[at], "-n")) {
        if (++at == argc || !count_arg(argv[at++], &lines)) return error("expected a line count");
    } else if (at < argc && argv[at][0] == '-' && count_arg(argv[at] + 1, &lines)) ++at;
    if (at < argc && equal(argv[at], "--")) ++at;
    if (argc - at > 1) return error("usage: head|tail [-n COUNT] [FILE]");
    Input in;
    if (!input_open(&in, at < argc ? argv[at] : 0)) return error("file not found");
    int c;
    if (!tail) {
        while (lines && (c = input_byte(&in)) >= 0) {
            put_byte(c);
            if (c == '\n') --lines;
        }
    } else if (lines) {
        const unsigned capacity = 1024 * 1024;
        char *data = (char *)console_call(37, capacity);
        if ((long)data < 0) { input_close(&in); return error("out of memory"); }
        unsigned n = 0;
        while ((c = input_byte(&in)) >= 0) {
            if (n == capacity) { input_close(&in); return error("tail input exceeds 1 MiB"); }
            data[n++] = c;
        }
        unsigned from = n;
        if (from && data[from - 1] == '\n') --from;
        while (from) {
            if (data[from - 1] == '\n' && --lines == 0) break;
            --from;
        }
        output(data + from, n - from);
    }
    input_close(&in);
    return in.size < 0 ? error("input read failed") : 0;
}

static int space_byte(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

static void counts(unsigned flags, const unsigned long values[3], const char *label) {
    int seen = 0;
    for (unsigned i = 0; i < 3; ++i) {
        if (flags & (1u << i)) {
            if (seen++) put_byte(' ');
            number(values[i]);
        }
    }
    if (label) { put_byte(' '); output(label, length(label)); }
    put_byte('\n');
}

static int wc(int argc, char **argv) {
    unsigned flags = 0;
    int at = 0, failed = 0;
    while (at < argc && argv[at][0] == '-' && argv[at][1]) {
        const char *p = argv[at++] + 1;
        if (equal(p, "-")) break;
        while (*p) {
            if (*p == 'l') flags |= 1;
            else if (*p == 'w') flags |= 2;
            else if (*p == 'c') flags |= 4;
            else return error("usage: wc [-lwc] [FILE ...]");
            ++p;
        }
    }
    if (!flags) flags = 7;
    int files = argc - at;
    unsigned long total[3] = {0, 0, 0};
    do {
        const char *path = at < argc ? argv[at] : 0;
        unsigned long values[3] = {0, 0, 0};
        Input in;
        if (!input_open(&in, path)) { failed = error("file not found"); }
        else {
            int c, word = 0;
            while ((c = input_byte(&in)) >= 0) {
                int next = !space_byte(c);
                values[0] += c == '\n';
                values[1] += next && !word;
                ++values[2];
                word = next;
            }
            input_close(&in);
            if (in.size < 0) failed = error("input read failed");
            else {
                counts(flags, values, path);
                for (int i = 0; i < 3; ++i) total[i] += values[i];
            }
        }
    } while (++at < argc);
    if (files > 1) counts(flags, total, "total");
    return failed;
}

static int tee(int argc, char **argv) {
    int append = argc && equal(argv[0], "-a");
    int at = append;
    if (at < argc && equal(argv[at], "--")) ++at;
    int fds[16], used = 0, failed = 0;
    if (argc - at > 16) return error("tee supports at most 16 output files");
    for (; at < argc; ++at) {
        int fd = lx_open_write(argv[at], append);
        if (fd < 0) failed = error("cannot open output file");
        else fds[used++] = fd;
    }
    int c;
    char block[4096];
    unsigned n = 0;
    while ((c = lx_getchar()) >= 0) {
        put_byte(c);
        block[n++] = c;
        if (n == sizeof(block)) {
            for (int i = 0; i < used; ++i)
                if (lx_write(fds[i], block, n) != (int)n) failed = 1;
            n = 0;
        }
    }
    for (int i = 0; i < used; ++i) {
        if (n && lx_write(fds[i], block, n) != (int)n) failed = 1;
        lx_close(fds[i]);
    }
    if (failed) return error("tee could not write all output");
    return 0;
}

typedef struct { const char *text; unsigned size; } Line;

static int line_compare(Line a, Line b) {
    unsigned n = a.size < b.size ? a.size : b.size;
    for (unsigned i = 0; i < n; ++i) {
        int x = (unsigned char)a.text[i], y = (unsigned char)b.text[i];
        if (x != y) return x - y;
    }
    return (a.size > b.size) - (a.size < b.size);
}

static void sorted_lines(Line *lines, Line *scratch, unsigned count, int reverse) {
    for (unsigned stride = 1; stride < count; stride *= 2) {
        for (unsigned start = 0; start < count; start += stride * 2) {
            unsigned mid = start + stride;
            if (mid > count) mid = count;
            unsigned end = mid + stride;
            if (end > count) end = count;
            unsigned left = start, right = mid;
            for (unsigned out = start; out < end; ++out) {
                int order = left == mid || right == end ? 0 : line_compare(lines[left], lines[right]);
                if (left < mid && (right == end || (reverse ? order >= 0 : order <= 0)))
                    scratch[out] = lines[left++];
                else
                    scratch[out] = lines[right++];
            }
        }
        for (unsigned i = 0; i < count; ++i) lines[i] = scratch[i];
    }
}

static int sort_uniq(int argc, char **argv, int sort) {
    int unique = !sort, reverse = 0, count_repeats = 0, at = 0;
    while (at < argc && argv[at][0] == '-' && argv[at][1]) {
        const char *flags = argv[at++] + 1;
        if (equal(flags, "-")) break;
        for (; *flags; ++flags) {
            if (sort && *flags == 'u') unique = 1;
            else if (sort && *flags == 'r') reverse = 1;
            else if (!sort && *flags == 'c') count_repeats = 1;
            else return error(sort ? "usage: sort [-ru] [FILE]" : "usage: uniq [-c] [FILE]");
        }
    }
    if (argc - at > 1) return error(sort ? "usage: sort [-ru] [FILE]" : "usage: uniq [-c] [FILE]");
    Input in;
    if (!input_open(&in, at < argc ? argv[at] : 0)) return error("file not found");
    const unsigned capacity = 1024 * 1024;
    char *data = (char *)console_call(37, capacity);
    if ((long)data < 0) { input_close(&in); return error("out of memory"); }
    unsigned len = 0, count = 0;
    int c;
    while ((c = input_byte(&in)) >= 0) {
        if (len == capacity) { input_close(&in); return error("input exceeds 1 MiB"); }
        data[len++] = c;
        if (c == '\n') ++count;
    }
    input_close(&in);
    if (in.size < 0) return error("input read failed");
    if (len && data[len - 1] != '\n') ++count;
    if (!count) return 0;
    if (console_call(37, capacity + sizeof(Line) * (1 + sort) * count) < 0)
        return error("out of memory");
    Line *lines = (Line *)(data + capacity);
    unsigned from = 0, line = 0;
    for (unsigned i = 0; i < len; ++i) {
        if (data[i] == '\n') {
            lines[line++] = (Line){data + from, i - from};
            from = i + 1;
        }
    }
    if (from < len) lines[line] = (Line){data + from, len - from};
    if (sort) sorted_lines(lines, lines + count, count, reverse);
    for (unsigned i = 0; i < count;) {
        unsigned next = i + 1;
        while (unique && next < count && line_compare(lines[i], lines[next]) == 0) ++next;
        if (count_repeats) { number(next - i); put_byte(' '); }
        output(lines[i].text, lines[i].size);
        put_byte('\n');
        i = next;
    }
    return 0;
}

#include "../file-time.h"

static void utc_date(unsigned seconds) {
    char text[24];
    format_utc(text, seconds);
    output(text, sizeof text - 1);
}

static int glob(const char *pattern, const char *name) {
    const char *star = 0, *after = 0;
    while (*name) {
        if (*pattern == '*') { star = pattern++; after = name; }
        else if (*pattern == '?' || *pattern == *name) { ++pattern; ++name; }
        else if (star) { pattern = star + 1; name = ++after; }
        else return 0;
    }
    while (*pattern == '*') ++pattern;
    return !*pattern;
}

typedef int (*Visitor)(const char *path, const char *leaf, long kind, unsigned depth, void *state);

static int walk_paths(const char *root, unsigned limit, Visitor visit, void *state) {
    unsigned len = length(root), depth = 0;
    if (!len || len > 512) return error("invalid path length");
    long kind = lx_stat(root);
    if (kind < 0 && kind != LX_DIR) return error("path not found");
    char path[513], name[513];
    while (len > 1 && root[len - 1] == '/') --len;
    for (unsigned i = 0; i < len; ++i) path[i] = root[i];
    path[len] = 0;
    struct { unsigned len, next; } dirs[257];
    for (;;) {
        const char *leaf = path;
        for (unsigned i = 0; i + 1 < len; ++i)
            if (path[i] == '/') leaf = path + i + 1;
        if (visit(path, leaf, kind, depth, state)) return 1;
        if (kind == LX_DIR && depth < limit) {
            if (depth == sizeof(dirs)/sizeof(*dirs)) return error("directory nesting too deep");
            dirs[depth].len = len;
            dirs[depth++].next = 0;
        }
        for (;;) {
            if (!depth) return 0;
            len = dirs[depth - 1].len;
            path[len] = 0;
            kind = lx_dir(path, dirs[depth - 1].next++, name);
            if (kind == LX_END) { --depth; continue; }
            if (kind < 0 && kind != LX_DIR) return error("directory read failed");
            unsigned size = length(name);
            if (len + (path[len - 1] != '/') + size > 512) return error("path too long");
            if (path[len - 1] != '/') path[len++] = '/';
            for (unsigned i = 0; i < size; ++i) path[i + len] = name[i];
            len += size;
            path[len] = 0;
            break;
        }
    }
}

typedef struct { const char *pattern; unsigned min_depth; int type, nul; } Find;

static int find_visit(const char *path, const char *leaf, long kind, unsigned depth, void *state) {
    Find *options = state;
    if (depth >= options->min_depth
        && (!options->type || (kind == LX_DIR ? options->type == 'd' : options->type == 'f'))
        && glob(options->pattern, leaf)) {
        output(path, length(path));
        put_byte(options->nul ? 0 : '\n');
    }
    return 0;
}

static int find(int argc, char **argv) {
    const char *root = ".";
    Find options = {"*", 0, 0, 0};
    unsigned limit = 512;
    int at = 0;
    if (at < argc && argv[at][0] != '-') root = argv[at++];
    while (at < argc) {
        const char *option = argv[at++];
        if (at < argc && equal(option, "-name")) options.pattern = argv[at++];
        else if (equal(option, "-print0")) options.nul = 1;
        else if (equal(option, "-print")) options.nul = 0;
        else if (at < argc && equal(option, "-mindepth") && count_arg(argv[at], &options.min_depth)) ++at;
        else if (at < argc && equal(option, "-maxdepth") && count_arg(argv[at], &limit)) ++at;
        else if (at < argc && equal(option, "-type") && (equal(argv[at], "d") || equal(argv[at], "f")))
            options.type = argv[at++][0];
        else return error("usage: find [PATH] [-name PATTERN] [-type f|d] [-mindepth N] [-maxdepth N] [-print|-print0]");
    }
    return walk_paths(root, limit, find_visit, &options);
}
