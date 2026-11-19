// Disk-resident text utilities. The build selects one command per ELF.
#include "../console.h"
#include "../tcc/lixin.h"
#ifdef __TINYC__
#include <stdio.h>
#define lx_getchar getchar
#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)
#define COMMAND STRINGIFY(APP)
#endif
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
#include "text.h"
#include "discovery.h"
#include "transform.h"
#include "arguments.h"

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
static unsigned char folded(unsigned char byte, int ignore_case) {
    return ignore_case && byte >= 'A' && byte <= 'Z' ? byte + 'a' - 'A' : byte;
}

typedef struct {
    const char *pattern;
    char *buffer;
    int ignore_case, invert, line_number, with_path, count_only, list_only, found, failed;
} Search;

static int grep_file(Search *search, const char *path) {
    Input in;
    if (!input_open(&in, path)) { error("file not found"); return 2; }
    char *line = search->buffer;
    const char *pattern = search->pattern;
    unsigned n = 0, patlen = length(pattern);
    unsigned long line_id = 1, selected = 0;
    for (;;) {
        int c = input_byte(&in);
        if (c < 0 && !n)
            break;
        if (c >= 0 && c != '\n') {
            if (n == 1024 * 1024) {
                input_close(&in);
                error("input line too long");
                return 2;
            }
            line[n++] = c;
            continue;
        }
        if (c == '\n' && n && line[n - 1] == '\r')
            --n;
        int matched = 0;
        for (unsigned i = 0; i + patlen <= n; i++) {
            unsigned j = 0;
            while (j < patlen && folded(line[i + j], search->ignore_case) == folded(pattern[j], search->ignore_case))
                ++j;
            if (j == patlen) {
                matched = 1;
                break;
            }
        }
        if (matched != search->invert) {
            ++selected;
            search->found = 1;
            if (search->list_only) break;
            if (!search->count_only) {
                if (search->with_path && path) { output(path, length(path)); put_byte(':'); }
                if (search->line_number) { number(line_id); put_byte(':'); }
                output(line, n);
                put_byte('\n');
            }
        }
        ++line_id;
        n = 0;
        if (c < 0)
            break;
    }
    input_close(&in);
    if (in.size < 0) return 2;
    if (search->list_only && selected) {
        const char *name = path && !equal(path, "-") ? path : "(standard input)";
        output(name, length(name)); put_byte('\n');
    } else if (search->count_only) {
        if (search->with_path && path) { output(path, length(path)); put_byte(':'); }
        number(selected); put_byte('\n');
    }
    return 0;
}
static int grep_visit(const char *path, const char *leaf, long kind, unsigned depth, void *state) {
    (void)leaf; (void)depth;
    Search *search = state;
    if (kind == LX_DIR) return 0;
    Input in;
    if (!input_open(&in, path)) {
        error("file not found");
        search->failed = 1;
        return 0;
    }
    int c;
    do { c = input_byte(&in); } while (c > 0);
    input_close(&in);
    if (in.size < 0 || (c < 0 && grep_file(search, path))) search->failed = 1;
    return 0;
}

static int run(const char *cwd, int argc, char **argv) {
    if (equal(COMMAND, "echo")) {
        for (int i = 0; i < argc; ++i) {
            if (i) put_byte(' ');
            output(argv[i], length(argv[i]));
        }
        put_byte('\n');
        return 0;
    }
    if (equal(COMMAND, "ps") || equal(COMMAND, "df") || equal(COMMAND, "cpuinfo")) {
        if (argc) return error("usage: ps | df | cpuinfo");
        unsigned kind = equal(COMMAND, "ps") ? 6 : equal(COMMAND, "df") ? 7 : 8;
        long n = console_call(29, kind);
        if (n < 0) return error("system information unavailable");
        output((const char *)0x443d7000, n);
        if (n && ((const char *)0x443d7000)[n - 1] != '\n') put_byte('\n');
        return 0;
    }
    if (equal(COMMAND, "find")) return find(argc, argv);
    if (equal(COMMAND, "clear")) {
        if (argc)
            return 1;
        output("\033[2J\033[H", 7);
        return 0;
    }
    if (equal(COMMAND, "netinfo")) {
        if (argc) return error("usage: netinfo");
        long n = console_call(29, 0);
        if (n < 0) return error("eth0 unavailable");
        output((const char *)0x443d7000, n);
        return 0;
    }
    if (equal(COMMAND, "uname") || equal(COMMAND, "sysinfo")) {
        unsigned kind = 5;
        if (equal(COMMAND, "uname")) {
            kind = 1;
            if (argc) {
                if (argc != 1)
                    return error("usage: uname [-a|-s|-r|-m]");
                if (equal(argv[0], "-r")) kind = 2;
                else if (equal(argv[0], "-m")) kind = 3;
                else if (equal(argv[0], "-a")) kind = 4;
                else if (!equal(argv[0], "-s")) return error("usage: uname [-a|-s|-r|-m]");
            }
        } else if (argc) {
            return error("usage: sysinfo");
        }
        long n = console_call(29, kind);
        if (n < 0) return error("system information unavailable");
        output((const char *)0x443d7000, n);
        put_byte('\n');
        return 0;
    }
    if (equal(COMMAND, "pwd")) {
        if (argc)
            return error("pwd takes no arguments");
        output(cwd, length(cwd));
        put_byte('\n');
        return 0;
    }
    if (equal(COMMAND, "cat")) {
        int at = argc && equal(argv[0], "--");
        if (at == argc) return cat(0);
        int status = 0;
        for (; at < argc; ++at) status |= cat(equal(argv[at], "-") ? 0 : argv[at]);
        return status;
    }
    if (equal(COMMAND, "grep")) {
        int at = 0, recursive = 0;
        Search search = {0};
        const char *usage = "usage: grep [-Finvrcl] PATTERN [PATH ...]";
        while (at < argc && argv[at][0] == '-' && argv[at][1]) {
            const char *flag = argv[at++] + 1;
            if (equal(flag, "-")) break;
            for (; *flag; ++flag) {
                if (*flag == 'i') search.ignore_case = 1;
                else if (*flag == 'v') search.invert = 1;
                else if (*flag == 'n') search.line_number = 1;
            else if (*flag == 'c') search.count_only = 1;
            else if (*flag == 'l') search.list_only = 1;
                else if (*flag == 'r' || *flag == 'R') recursive = 1;
                else if (*flag != 'F') { error(usage); return 2; }
            }
        }
        if (at == argc) { error(usage); return 2; }
        search.pattern = argv[at++];
        search.with_path = recursive || argc - at > 1;
        search.buffer = (char *)console_call(37, 1024 * 1024);
        if ((long)search.buffer < 0) { error("out of memory"); return 2; }
        do {
            const char *path = at < argc ? argv[at] : (recursive ? "." : 0);
            if (recursive && path && !equal(path, "-")) {
                if (walk_paths(path, 512, grep_visit, &search)) search.failed = 1;
            } else if (grep_file(&search, path)) search.failed = 1;
        } while (++at < argc);
        return search.failed ? 2 : !search.found;
    }
    if (equal(COMMAND, "seq")) return sequence(argc, argv);
    if (equal(COMMAND, "test")) return test_expression(argc, argv);
    if (equal(COMMAND, "printf")) return printf_command(argc, argv);
    if (equal(COMMAND, "basename") || equal(COMMAND, "dirname"))
        return path_part(argc, argv, equal(COMMAND, "dirname"));
    if (equal(COMMAND, "stat")) return stat_paths(argc, argv);
    if (equal(COMMAND, "tree")) return tree(argc, argv);
    if (equal(COMMAND, "du")) return du(argc, argv);
    if (equal(COMMAND, "cmp") || equal(COMMAND, "diff"))
        return compare_files(argc, argv, equal(COMMAND, "diff"));
    if (equal(COMMAND, "nl")) return nl(argc, argv);
    if (equal(COMMAND, "cut")) return cut(argc, argv);
    if (equal(COMMAND, "tr")) return tr(argc, argv);
    if (equal(COMMAND, "sed")) return sed(argc, argv);
    if (equal(COMMAND, "head") || equal(COMMAND, "tail"))
        return head_tail(argc, argv, equal(COMMAND, "tail"));
    if (equal(COMMAND, "sort") || equal(COMMAND, "uniq"))
        return sort_uniq(argc, argv, equal(COMMAND, "sort"));
    if (equal(COMMAND, "tee")) return tee(argc, argv);
    if (equal(COMMAND, "wc")) return wc(argc, argv);
    if (equal(COMMAND, "true") || equal(COMMAND, "false")) return equal(COMMAND, "false");
    if (equal(COMMAND, "sleep")) {
        unsigned seconds;
        if (argc != 1 || !count_arg(argv[0], &seconds) || seconds > 3600)
            return error("usage: sleep SECONDS (0-3600)");
        return console_call(43, (unsigned long)seconds * 1000) < 0;
    }
    if (equal(COMMAND, "date")) {
        if (argc > 1 || (argc == 1 && !equal(argv[0], "+%s"))) return error("usage: date [+%s]");
        long seconds = console_call(36, 0);
        if (seconds < 0) return error("clock unavailable");
        if (argc) number(seconds);
        else utc_date(seconds);
        put_byte('\n');
        return 0;
    }
    return error("unknown command");
}

#ifdef __TINYC__
int main(int argc, char **argv) {
    char *mail = (char *)0x443d7000;
    mail[0] = '.'; mail[1] = 0;
    if (console_call(23, 0)) return error("cannot resolve working directory");
    char cwd[513];
    unsigned i;
    for (i = 0; mail[i]; ++i) cwd[i] = mail[i];
    cwd[i] = 0;
    return run(cwd, argc - 1, argv + 1);
}
#else
__attribute__((section(".text.start"), noreturn))
void _start(const char *cwd, const char *args, unsigned long len) {
    char *argv[512];
    int argc = 0;
    const char *end = args + len;
    while (args < end) {
        if (argc == 512) finish(1);
        argv[argc++] = (char *)args;
        args += length(args) + 1;
    }
    finish(run(cwd, argc, argv));
}
#endif
