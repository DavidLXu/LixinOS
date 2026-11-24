// Reuse the guest shell's lexer, pipeline executor, environment and builtins.
#define main bash_main
#include "/src/bash/bash.c"
#undef main

static int read_text(const char *path, char *buffer, int capacity) {
    long size = lx_stat(path);
    if (size < 0 || size >= capacity) return -1;
    int fd = lx_open(path), used = 0;
    if (fd < 0) return -1;
    while (used < size) {
        int count = lx_read(fd, buffer + used, size - used);
        if (count <= 0) { lx_close(fd); return -1; }
        used += count;
    }
    lx_close(fd);
    buffer[used] = 0;
    return 0;
}

// The base volume launches LXE programs. Dispatch explicit shell-script paths
// through the guest interpreter, including stages of a pipeline.
static void script_stages(Session *session, Tokens *tokens) {
    for (int i = 0; i < tokens->count; ++i) {
        if ((i && tokens->items[i-1].kind != PIPE) || tokens->items[i].kind != WORD) continue;
        char *path = tokens->items[i].text;
        if (!strchr(path, '/')) continue;
        char full[513];
        if (path[0] != '/') {
            if (snprintf(full, sizeof(full), "%s/%s", session->cwd, path) >= sizeof(full)) continue;
            path = full;
        }
        if (lx_stat(path) < 2) continue;
        char line[48];
        int fd = lx_open(path);
        if (fd < 0) continue;
        int n = lx_read(fd, line, sizeof(line)-1);
        lx_close(fd);
        if (n < 2) continue;
        line[n] = 0;
        char *newline = strchr(line, '\n');
        if (newline) *newline = 0;
        if (!equal(line, "#!/bin/sh") && !equal(line, "#!/bin/bash") &&
            !equal(line, "#!/usr/bin/env bash")) continue;
        if (tokens->count >= LINE+2) continue;
        for (int j = tokens->count; j > i; --j) tokens->items[j] = tokens->items[j-1];
        tokens->items[i].text = copy("/bin/sh");
        ++tokens->count;
        ++i;
    }
}

// Keep one shell session for command lists so cd, variables and $? persist.
static int run_list(Session *s, char *script) {
    char *start = script;
    int quote = 0, run = 1, need_command = 0, comment = 0;
    for (char *p = script;; ++p) {
        int c = *p;
        if (comment && c && c != '\n') { *p = ' '; continue; }
        if (c == '\n') comment = 0;
        if (!quote && c == '#' && (p == start || p[-1] == ' ' || p[-1] == '\t')) {
            comment = 1; *p = ' '; continue;
        }
        if (c == '\\' && quote != 39 && p[1]) { ++p; continue; }
        if (c && c == quote) { quote = 0; continue; }
        if (!quote && (c == 39 || c == '"')) { quote = c; continue; }
        int pair = !quote && (c == '&' || c == '|') && p[1] == c;
        if (c && (quote || (!pair && c != ';' && c != '\n'))) continue;
        *p = 0;
        Tokens tokens = {0};
        int result = strlen(start) > LINE ? fail("shell segment exceeds 512 bytes") :
                     lex(s, start, &tokens, 0, 0);
        if (result >= 0 && !tokens.count && (pair || (need_command && c != '\n')))
            result = fail("missing command");
        if (result >= 0 && tokens.count && tokens.items[tokens.count-1].kind == BACKGROUND)
            result = fail("background jobs are unavailable");
        if (result >= 0 && tokens.count && run) {
            script_stages(s, &tokens);
            result = execute(s, &tokens);
        }
        int had_words = tokens.count;
        clear_tokens(&tokens);
        if (result < 0) { printf("shell: %s\n", error); s->status = 1; }
        if (result == 1 || !c) return s->status;
        if (had_words || pair) {
            run = !pair || (c == '&' ? !s->status : !!s->status);
            need_command = pair;
        }
        if (pair) ++p;
        start = p + 1;
    }
}

int main(int argc, char **argv) {
    if (argc != 2 || !equal(argv[1], "--codex-job")) {
        char source[8193];
        char *script = source;
        int first_arg = 1;
        if (argc == 3 && equal(argv[1], "-c")) {
            if (strlen(argv[2]) >= sizeof(source)) return 2;
            strcpy(source, argv[2]);
            first_arg = argc;
        } else if (argc >= 2 && argv[1][0] != '-') {
            if (read_text(argv[1], source, sizeof(source))) { lx_error("cannot read shell script\n"); return 1; }
        } else return bash_main(argc, argv);
        char *cwd = query(CHDIR, 0, ".");
        if (!cwd) return 1;
        Session *session = new_session(1, cwd);
        for (int i = first_arg; i < argc; ++i) {
            char key[16]; snprintf(key, sizeof(key), "%d", i-first_arg);
            if (assign(session, key, argv[i]) < 0) { drop_session(session); return 1; }
        }
        int status = run_list(session, script);
        drop_session(session);
        return status;
    }
    char cwd[513], script[8193], marker[64];
    if (read_text("/job.cwd", cwd, sizeof(cwd)) || read_text("/job.cmd", script, sizeof(script)) ||
        read_text("/job.marker", marker, sizeof(marker))) return 125;
    char *resolved = query(CHDIR, 0, cwd);
    if (!resolved) return 125;
    Session *s = new_session(1, resolved);
    int status = run_list(s, script);
    drop_session(s);
    unsigned char result = status;
    if (lx_save_file("/job.status", &result, 1)) return 125;
    printf("\n%s\n", marker);
    return status;
}
