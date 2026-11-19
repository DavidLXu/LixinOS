#include "../console.h"
#include "../tcc/lixin.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

extern int compile_rust_image(const char *path, const char *source, uint64_t *result);
#define PATH_MAIL ((char *)0x443d7000)
static char *resolved(const char *name) {
    char *mail = PATH_MAIL;
    if (strlen(name) > 512) return NULL;
    strcpy(mail, name);
    return console_call(23, 0) == 0 ? strdup(mail) : NULL;
}
extern int lust_compile(const char *, const char *, void *);
extern int lx_save_parts(const char *, unsigned long);
static char *c_output;
static const char *exported;
const char *export_name(void) { return exported; }
int save_generated_c(const unsigned char *source, size_t size) {
    if (!size || size > 1024 * 1024) return -1;
    // The kernel reads the descriptor from the writable data segment.
    static uint32_t table[] = {1, 0, 0, 0};
    table[2] = (uintptr_t)source;
    table[3] = size;
    return lx_save_parts(c_output, (uint64_t)(uintptr_t)table << 32 | size);
}
static int build(char *src, char *dst, int emit_c) {
    char *input = resolved(src), *output = resolved(dst);
    if (!input || !output || !strcmp(input, output)) {
        lx_error("lust: invalid paths or output would overwrite source\n");
        return 1;
    }
    long len = lx_stat(input);
    if (len < 0 || len > 32768) {
        lx_error("lust: expected a source file of at most 32 KiB\n");
        return 1;
    }
    char *source = malloc(len + 1);
    int fd = lx_open(input);
    if (!source || fd < 0) return 1;
    for (long offset = 0; offset < len;) {
        int n = lx_read(fd, source + offset, len - offset);
        if (n <= 0) { lx_error("lust: source read failed\n"); return 1; }
        offset += n;
    }
    lx_close(fd);
    if (memchr(source, 0, len)) { lx_error("lust: NUL in source\n"); return 1; }
    source[len] = 0;
    if (emit_c) {
        c_output = output;
        return lust_compile(input, source, NULL);
    }
    uint64_t result;
    if (compile_rust_image(input, source, &result)) return 1;
    strcpy(PATH_MAIL, output);
    if (console_call(22, result)) {
        lx_error("lust: could not save executable\n");
        return 1;
    }
    return 0;
}
extern char __data_image[], __data_begin[], __data_end[];
__attribute__((section(".text.start"), noreturn))
void _start(const char *cwd, char *args, size_t len) {
    (void)cwd;
    memcpy(__data_begin, __data_image, __data_end - __data_begin);
    char *a[5], *p = args, *end = args + len;
    int n = 0;
    while (p < end && n < 5) {
        a[n] = strdup(p);
        if (!a[n++]) finish(1);
        p += strlen(p) + 1;
    }
    if (p == end && n == 5 && !strcmp(a[0], "emit-fn") && !strcmp(a[3], "-o")) {
        exported = a[1];
        finish(build(a[2], a[4], 1));
    }
    if (p != end || n != 4 || (strcmp(a[0], "build") && strcmp(a[0], "emit-c")) || strcmp(a[2], "-o")) {
        lx_error("usage: lust build source.rs -o program | lust emit-c source.rs -o source.c\n");
        finish(2);
    }
    finish(build(a[1], a[3], !strcmp(a[0], "emit-c")));
}
