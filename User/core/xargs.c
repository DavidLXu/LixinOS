#include <lixin.h>
#include <stdio.h>
#include <string.h>
#define length strlen
#define equal(a, b) (!strcmp(a, b))
#define lx_getchar getchar

static int error(const char *message) {
    lx_error(message);
    lx_error("\n");
    return 1;
}

static int count_arg(const char *text, unsigned *value) {
    unsigned n = 0;
    if (!*text) return 0;
    while (*text >= '0' && *text <= '9' && n <= 64) n = n * 10 + (*text++ - '0');
    if (*text) return 0;
    *value = n;
    return 1;
}

int main(int argc, char **argv) {
    --argc; ++argv;
    int at = 0, nul = 0;
    unsigned limit = 32;
    while (at < argc) {
        if (equal(argv[at], "-0")) { nul = 1; ++at; }
        else if (equal(argv[at], "-n")) {
            if (++at == argc || !count_arg(argv[at++], &limit) || !limit || limit > 64)
                return error("expected batch size 1 to 64");
        } else break;
    }
    if (at < argc && equal(argv[at], "--")) ++at;
    if (at == argc || argc - at > 32) return error("usage: xargs [-0] [-n N] COMMAND [ARG ...]");
    char path[513];
    const char *name = argv[at];
    unsigned n = length(name), slash = 0;
    for (unsigned i = 0; i < n; ++i) slash |= name[i] == '/';
    unsigned prefix = slash ? 0 : 5;
    if (n + prefix >= sizeof(path)) return error("command path too long");
    if (prefix) for (unsigned i = 0; i < prefix; ++i) path[i] = "/bin/"[i];
    for (unsigned i = 0; i <= n; ++i) path[prefix + i] = name[i];
    char *args[97], values[64][513];
    int fixed = argc - at;
    unsigned fixed_size = 0;
    for (int i = 0; i < fixed; ++i) {
        args[i] = argv[at+i];
        fixed_size += length(args[i]) + 1;
    }
    if (fixed_size > 32768 - sizeof(values[0])) return error("fixed arguments too large");
    unsigned used = 0, len = 0, packed_size = fixed_size;
    int c, status = 0;
    do {
        c = lx_getchar();
        if (c >= 0 && !(nul ? c == 0 : (c == ' ' || (c >= '\t' && c <= '\r')))) {
            if (len == 512 || !c) return error("invalid or oversized xargs argument");
            values[used][len++] = c;
            continue;
        }
        if (len || (nul && c == 0)) {
            values[used][len] = 0;
            args[fixed + used] = values[used];
            ++used;
            packed_size += len + 1;
            len = 0;
        }
        if (used && (used == limit || c < 0 || packed_size > 32768 - sizeof(values[0]))) {
            int result = lx_spawn(path, fixed + used, args);
            if (result < 0) return error("could not run xargs command");
            if (result) status = 1;
            used = 0;
            packed_size = fixed_size;
        }
    } while (c >= 0);
    return status;
}
