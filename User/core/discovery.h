static void line(const char *s) {
    output(s, length(s));
    put_byte('\n');
}

static int stat_paths(int argc, char **argv) {
    if (!argc) return error("usage: stat PATH ...");
    int failed = 0;
    for (int i = 0; i < argc; ++i) {
        long kind = lx_stat(argv[i]);
        if (kind < 0 && kind != LX_DIR) {
            lx_error(argv[i]); lx_error(": ");
            failed = error("path not found");
            continue;
        }
        unsigned birth = lx_file_time(0), modified = lx_file_time(1);
        output("Path: ", 6); line(argv[i]);
        if (kind == LX_DIR) line("Type: directory");
        else {
            line("Type: regular file");
            output("Size: ", 6); number(kind); line(" bytes");
        }
        output("Modify: ", 8);
        if (modified) { utc_date(modified); put_byte('\n'); }
        else line("-");
        output("Birth: ", 7);
        if (birth) { utc_date(birth); put_byte('\n'); }
        else line("-");
    }
    return failed;
}

static int tree_visit(const char *path, const char *leaf, long kind, unsigned depth, void *state) {
    (void)state;
    for (unsigned n = 1; n < depth; ++n) output("    ", 4);
    if (depth) output("|-- ", 4);
    const char *name = depth ? leaf : path;
    output(name, length(name));
    if (kind == LX_DIR && !equal(name, "/")) put_byte('/');
    put_byte('\n');
    return 0;
}

static int tree(int argc, char **argv) {
    unsigned limit = 512;
    int at = 0;
    if (argc && equal(argv[0], "-L")) {
        if (argc < 2 || !count_arg(argv[1], &limit)) return error("expected a depth");
        at = 2;
    }
    if (argc - at > 1) return error("usage: tree [-L N] [PATH]");
    return walk_paths(at < argc ? argv[at] : ".", limit, tree_visit, 0);
}

static int du_visit(const char *path, const char *leaf, long kind, unsigned depth, void *state) {
    (void)path; (void)leaf; (void)depth;
    if (kind >= 0) *(unsigned long *)state += kind;
    return 0;
}

static int du(int argc, char **argv) {
    int at = 0, failed = 0;
    if (argc && (equal(argv[0], "-s") || equal(argv[0], "-b") || equal(argv[0], "-sb"))) ++at;
    if (at < argc && equal(argv[at], "--")) ++at;
    do {
        const char *path = at < argc ? argv[at] : ".";
        unsigned long bytes = 0;
        if (walk_paths(path, 512, du_visit, &bytes)) failed = 1;
        else { number(bytes); put_byte('\t'); line(path); }
    } while (++at < argc);
    return failed;
}

static int compare_files(int argc, char **argv, int diff) {
    int quiet = argc && equal(argv[0], diff ? "-q" : "-s");
    int at = quiet || (diff && argc && equal(argv[0], "-u"));
    if (at < argc && equal(argv[at], "--")) ++at;
    if (argc - at != 2 || (equal(argv[at], "-") && equal(argv[at + 1], "-"))) {
        error(diff ? "usage: diff [-u|-q] FILE FILE" : "usage: cmp [-s] FILE FILE");
        return 2;
    }
    const char *first = argv[at], *second = argv[at + 1];
    Input a, b;
    if (!input_open(&a, first)) { error("cannot open first file"); return 2; }
    if (!input_open(&b, second)) { input_close(&a); error("cannot open second file"); return 2; }
    int different = 0;
    if (!diff || quiet) {
        unsigned long pos = 1, row = 1;
        for (;;) {
            int x = input_byte(&a), y = input_byte(&b);
            if (a.size < 0 || b.size < 0) break;
            if (x != y) {
                different = 1;
                if (!quiet) {
                    output("Files differ at byte ", 21); number(pos);
                    output(", line ", 7); number(row); put_byte('\n');
                } else if (diff) line("Files differ");
                break;
            }
            if (x < 0) break;
            ++pos;
            if (x == '\n') ++row;
        }
    } else {
        const unsigned capacity = 1024 * 1024;
        char *data = (char *)console_call(37, 2 * capacity);
        if ((long)data < 0) {
            input_close(&a); input_close(&b);
            error("out of memory"); return 2;
        }
        unsigned sizes[2] = {0, 0}, rows[2] = {0, 0};
        int binary = 0, too_large = 0;
        Input *inputs[] = {&a, &b};
        for (unsigned i = 0; i < 2; ++i) {
            int c;
            while ((c = input_byte(inputs[i])) >= 0) {
                if (sizes[i] == capacity) { too_large = 1; break; }
                data[i * capacity + sizes[i]++] = c;
                rows[i] += c == '\n';
                binary |= c == 0;
            }
            if (sizes[i] && data[i * capacity + sizes[i] - 1] != '\n') ++rows[i];
        }
        if (too_large) {
            input_close(&a); input_close(&b);
            error("diff input exceeds 1 MiB"); return 2;
        }
        different = sizes[0] != sizes[1];
        for (unsigned i = 0; !different && i < sizes[0]; ++i)
            different = data[i] != data[capacity + i];
        if (different && binary) line("Binary files differ");
        if (different && !binary && a.size >= 0 && b.size >= 0) {
            output("--- ", 4); line(first);
            output("+++ ", 4); line(second);
            output("@@ -", 4); number(rows[0] != 0); put_byte(','); number(rows[0]);
            output(" +", 2); number(rows[1] != 0); put_byte(','); number(rows[1]); line(" @@");
            for (unsigned i = 0; i < 2; ++i) {
                char *text = data + i * capacity;
                for (unsigned j = 0; j < sizes[i]; ++j) {
                    if (!j || text[j - 1] == '\n') put_byte(i ? '+' : '-');
                    put_byte(text[j]);
                }
                if (sizes[i] && text[sizes[i] - 1] != '\n') {
                    put_byte('\n'); line("\\ No newline at end of file");
                }
            }
        }
    }
    input_close(&a); input_close(&b);
    if (a.size < 0 || b.size < 0) { error("file read failed"); return 2; }
    return different;
}
