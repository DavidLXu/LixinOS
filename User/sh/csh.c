// Small foreground shell that can be edited and rebuilt within LixinOS.
#include <stdio.h>
#include <lixin.h>

#ifndef CSH_PROMPT
#define CSH_PROMPT "csh$ "
#endif

static int equal(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

int main(void) {
    char line[513], path[513], *args[64];
    for (;;) {
        printf("%s", CSH_PROMPT);
        int n = 0, c, overflow = 0;
        while ((c = getchar()) != EOF && c != '\n' && c != '\r') {
            if (c == 8 || c == 127) {
                if (n) { --n; printf("\b \b"); }
            } else if (n < 512) {
                line[n++] = c;
                putchar(c);
            } else {
                overflow = 1;
            }
        }
        putchar('\n');
        if (c == EOF) return 0;
        if (overflow) { puts("command too long"); continue; }
        line[n] = 0;
        int argc = 0;
        char *p = line;
        while (*p) {
            while (*p == ' ' || *p == '\t') ++p;
            if (!*p || argc == 64) break;
            args[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') ++p;
            if (*p) *p++ = 0;
        }
        if (*p) { puts("too many arguments"); continue; }
        if (!argc) continue;
        char *cmd = args[0];
        if (equal(cmd, "exit")) return 0;
        char *filename = cmd;
        for (p = cmd; *p && *p != '/'; ++p) {}
        if (!*p) {
            if (snprintf(path, sizeof(path), "/bin/%s", cmd) >= sizeof(path)) {
                puts("path too long");
                continue;
            }
            filename = path;
        }
        int status = lx_spawn(filename, argc, args);
        if (status < 0) puts("could not start command");
        else if (status) printf("[exit %d]\n", status);
    }
}
