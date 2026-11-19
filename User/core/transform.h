static int selected_lines(Input *in, unsigned first, unsigned last, int numbered) {
    unsigned row = 1;
    int start = 1, c;
    while (row <= last && (c = input_byte(in)) >= 0) {
        if (row >= first) {
            if (start && numbered) { number(row); put_byte('\t'); }
            put_byte(c);
        }
        start = c == '\n';
        if (start) ++row;
    }
    return in->size < 0 ? error("input read failed") : 0;
}

static int nl(int argc, char **argv) {
    if (argc > 1) return error("usage: nl [FILE]");
    Input in;
    if (!input_open(&in, argc ? argv[0] : 0)) return error("file not found");
    int status = selected_lines(&in, 1, ~0u, 1);
    input_close(&in);
    return status;
}

static int cut(int argc, char **argv) {
    unsigned field;
    if (argc < 4 || argc > 5 || !equal(argv[0], "-d") || length(argv[1]) != 1
        || !equal(argv[2], "-f") || !count_arg(argv[3], &field) || !field)
        return error("usage: cut -d CHAR -f N [FILE]");
    Input in;
    if (!input_open(&in, argc == 5 ? argv[4] : 0)) return error("file not found");
    const unsigned limit = 1024 * 1024;
    char *prefix = (char *)console_call(37, limit);
    if ((long)prefix < 0) { input_close(&in); return error("out of memory"); }
    unsigned current = 1, stored = 0;
    int separator = (unsigned char)argv[1][0], c, started = 0;
    while ((c = input_byte(&in)) >= 0) {
        if (c == '\n') {
            if (current == 1 && field != 1) output(prefix, stored);
            put_byte('\n');
            current = 1; stored = 0; started = 0;
        } else if (c == separator) {
            ++current;
            started = 1;
        } else {
            started = 1;
            if (current == field) put_byte(c);
            else if (current == 1) {
                if (stored == limit) { input_close(&in); return error("input line exceeds 1 MiB"); }
                prefix[stored++] = c;
            }
        }
    }
    if (started) {
        if (current == 1 && field != 1) output(prefix, stored);
        put_byte('\n');
    }
    input_close(&in);
    return in.size < 0 ? error("input read failed") : 0;
}

static int expand_set(const unsigned char *text, unsigned char set[512]) {
    unsigned n = 0;
    while (*text) {
        unsigned first = *text++, last = first;
        if (text[0] == '-' && text[1]) {
            last = text[1];
            text += 2;
            if (last < first) return -1;
        }
        if (n + last - first >= 512) return -1;
        for (unsigned c = first; c <= last; ++c) set[n++] = c;
    }
    return n;
}

static int tr(int argc, char **argv) {
    int drop = argc && equal(argv[0], "-d");
    if (argc != 2) return error("usage: tr SET SET | tr -d SET");
    unsigned char from[512], to[512];
    int count = expand_set((const unsigned char *)argv[drop ? 1 : 0], from);
    int size = drop ? 0 : expand_set((const unsigned char *)argv[1], to);
    if (count < 0 || (!drop && size <= 0)) return error("invalid translation set");
    int mapped[256];
    for (int i = 0; i < 256; ++i) mapped[i] = i;
    for (int i = 0; i < count; ++i)
        mapped[from[i]] = drop ? -1 : to[i < size ? i : size - 1];
    int c;
    while ((c = lx_getchar()) >= 0)
        if (mapped[c] >= 0) put_byte(mapped[c]);
    return 0;
}

static int sed(int argc, char **argv) {
    int numbered = argc && equal(argv[0], "-n");
    int at = numbered;
    const char *usage = "usage: sed 's/OLD/NEW/[g]' [FILE] | sed -n 'FIRST,LASTp' [FILE]";
    if (argc - at < 1 || argc - at > 2) return error(usage);
    const char *script = argv[at++];
    const char *file = at < argc ? argv[at] : 0;
    if (numbered) {
        unsigned first = 0, last = 0;
        const char *p = script;
        while (*p >= '0' && *p <= '9' && first <= 100000000) first = first*10 + (*p++ - '0');
        last = first;
        if (*p == ',') {
            ++p; last = 0;
            while (*p >= '0' && *p <= '9' && last <= 100000000) last = last*10 + (*p++ - '0');
        }
        if (!first || last < first || *p != 'p' || p[1]) return error(usage);
        Input in;
        if (!input_open(&in, file)) return error("file not found");
        int status = selected_lines(&in, first, last, 0);
        input_close(&in);
        return status;
    }
    if (*script != 's' || !script[1]) return error(usage);
    const char delimiter = script[1], *old = script + 2, *replacement = old;
    while (*replacement && *replacement != delimiter) ++replacement;
    unsigned oldlen = replacement - old;
    if (!*replacement || !oldlen) return error(usage);
    const char *end = ++replacement;
    while (*end && *end != delimiter) ++end;
    if (!*end || (end[1] && !equal(end + 1, "g"))) return error(usage);
    unsigned newlen = end - replacement;
    int global = end[1] == 'g', changed = 0;
    const unsigned limit = 1024 * 1024;
    char *text = (char *)console_call(37, limit);
    if ((long)text < 0) return error("out of memory");
    Input in;
    if (!input_open(&in, file)) return error("file not found");
    unsigned len = 0;
    int c;
    do {
        c = input_byte(&in);
        if (c >= 0 && c != '\n') {
            if (len == limit) { input_close(&in); return error("input line exceeds 1 MiB"); }
            text[len++] = c;
            continue;
        }
        for (unsigned i = 0; i < len;) {
            unsigned j = 0;
            if ((!changed || global) && oldlen <= len - i)
                while (j < oldlen && text[i + j] == old[j]) ++j;
            if (j == oldlen) { output(replacement, newlen); i += oldlen; changed = 1; }
            else put_byte(text[i++]);
        }
        if (c == '\n') put_byte('\n');
        len = 0; changed = 0;
    } while (c >= 0);
    input_close(&in);
    return in.size < 0 ? error("input read failed") : 0;
}
