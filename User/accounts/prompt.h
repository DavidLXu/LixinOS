#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../console.h"

/* The foreground console byte ABI never echoes input. No line editor or
 * process argument retains the secret. Reject overlong lines in full. */
static char *read_secret(const char *prompt) {
    char line[256];
    size_t n = 0;
    int overflow = 0;
    fputs(prompt, stderr);
    fflush(stderr);
    for (;;) {
        int c = get_byte();
        if (c == '\r' || c == '\n') break;
        if (c <= 0 || c == 3 || c == 4) { overflow = 1; break; }
        if (c == 8 || c == 127) { if (n) --n; continue; }
        if (n + 1 < sizeof(line)) line[n++] = (char)c;
        else overflow = 1;
    }
    fputc('\n', stderr);
    line[n] = 0;
    char *out = overflow ? NULL : strdup(line);
    explicit_bzero(line, sizeof(line));
    return out;
}
static void discard_secret(char *secret) {
    if (secret) { explicit_bzero(secret, strlen(secret)); free(secret); }
}
