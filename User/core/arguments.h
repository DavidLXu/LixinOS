static int sequence(int argc, char **argv) {
    unsigned first = 1, step = 1, last;
    if (argc < 1 || argc > 3 || !count_arg(argv[argc - 1], &last)
        || (argc >= 2 && !count_arg(argv[0], &first))
        || (argc == 3 && (!count_arg(argv[1], &step) || !step)))
        return error("usage: seq [FIRST [STEP]] LAST (nonnegative integers)");
    for (unsigned n = first; n <= last; n += step) {
        number(n); put_byte('\n');
        if (last - n < step) break;
    }
    return 0;
}

static int path_part(int argc, char **argv, int directory) {
    if (argc != 1) return error(directory ? "usage: dirname PATH" : "usage: basename PATH");
    const char *path = argv[0];
    unsigned end = length(path);
    while (end > 1 && path[end - 1] == '/') --end;
    unsigned leaf = end;
    while (leaf && path[leaf - 1] != '/') --leaf;
    if (!directory) {
        if (end == 1 && path[0] == '/') line("/");
        else { output(path + leaf, end - leaf); put_byte('\n'); }
    } else if (!leaf) line(".");
    else {
        while (leaf > 1 && path[leaf - 1] == '/') --leaf;
        output(path, leaf); put_byte('\n');
    }
    return 0;
}

static int test_expression(int argc, char **argv) {
    int invert = argc > 1 && equal(argv[0], "!");
    argc -= invert;
    argv += invert;
    int result = 0;
    if (argc == 1) result = !!*argv[0];
    else if (argc == 2) {
        const char *op = argv[0], *arg = argv[1];
        if (equal(op, "-n")) result = !!*arg;
        else if (equal(op, "-z")) result = !*arg;
        else if (equal(op, "-e") || equal(op, "-f") || equal(op, "-d") || equal(op, "-s")) {
            long size = lx_stat(arg);
            if (equal(op, "-e")) result = size >= 0 || size == LX_DIR;
            if (equal(op, "-f")) result = size >= 0;
            if (equal(op, "-d")) result = size == LX_DIR;
            if (equal(op, "-s")) result = size > 0;
        } else argc = -1;
    } else if (argc == 3 && (equal(argv[1], "=") || equal(argv[1], "!="))) {
        result = equal(argv[0], argv[2]) != equal(argv[1], "!=");
    } else if (argc) argc = -1;
    if (argc < 0) { error("unsupported test expression"); return 2; }
    return result == invert;
}

static int decimal(const char *text) {
    int minus = *text == '-';
    if (minus || *text == '+') ++text;
    if (!*text) return 0;
    unsigned long value = 0, limit = (~0ul >> 1) + minus;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') return 0;
        unsigned digit = *text - '0';
        if (value > (limit - digit) / 10) return 0;
        value = value * 10 + digit;
    }
    if (minus && value) put_byte('-');
    number(value);
    return 1;
}

static int printf_command(int argc, char **argv) {
    int at = argc && equal(argv[0], "--");
    if (at == argc) return error("usage: printf FORMAT [ARG ...]");
    const char *format = argv[at++];
    int start;
    do {
        start = at;
        for (const char *p = format; *p; ++p) {
            if (*p == '%') {
                if (p[1] == '%') { put_byte('%'); ++p; continue; }
                if (p[1] != 's' && p[1] != 'd') return error("printf supports %s, %d and %%");
                ++p;
                const char *value = at < argc ? argv[at++] : *p == 's' ? "" : "0";
                if (*p == 's') output(value, length(value));
                else if (!decimal(value)) return error("expected a signed decimal integer");
            } else {
                int c = *p;
                if (c == '\\') {
                    if (p[1] == 'n') { c = '\n'; ++p; }
                    else if (p[1] == 't') { c = '\t'; ++p; }
                    else if (p[1] == 'r') { c = '\r'; ++p; }
                    else if (p[1] == '\\') ++p;
                }
                put_byte(c);
            }
        }
    } while (at < argc && at > start);
    return 0;
}
