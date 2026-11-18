#include "../console.h"
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include "lixin.h"

int lx_getchar(void);
FILE *lx_stream(int fd) { return fd == 0 ? stdin : fd == 1 ? stdout : stderr; }

extern char __user_heap_start[], __user_heap_limit[];
void *_sbrk(ptrdiff_t increment) {
    static char *end = __user_heap_start;
    if (increment < 0 || (uintptr_t)increment > (uintptr_t)__user_heap_limit - (uintptr_t)end) {
        errno = ENOMEM; return (void *)-1;
    }
    char *old = end; end += increment; return old;
}
int _write(int fd, const char *p, int n) {
    if (fd != 1 && fd != 2) { errno=EBADF; return -1; }
    for (int i = 0; i < n; ++i) console_call(fd == 2 ? 11 : 1, (unsigned char)p[i]);
    return n;
}
void lx_abort(void) { finish(134); }
void _exit(int status) { finish(status); }
#ifdef LX_COMPILER
static const char *const headers[] = {
    "#define EOF (-1)\n"
    "typedef struct lx_file FILE; FILE *lx_stream(int);\n"
    "#define stdin (lx_stream(0))\n#define stdout (lx_stream(1))\n#define stderr (lx_stream(2))\n"
    "int snprintf(char*,unsigned long,const char*,...);\n"
    "int puts(const char*); int printf(const char*,...); int putchar(int); int getchar(void);\n"
    "int fprintf(FILE*,const char*,...); FILE *fopen(const char*,const char*);\n"
    "unsigned long fread(void*,unsigned long,unsigned long,FILE*); int fclose(FILE*);\n",
#include "lixin-header.inc"
};
static const char *const paths[] = {"/include/stdio.h", "/include/lixin.h"};
static int offsets[] = {-1, -1};
#endif
int _open(const char *path, int flags, ...) {
    if (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND)) { errno = EROFS; return -1; }
#ifdef LX_COMPILER
    for (int i = 0; i < 2; ++i) {
        if (!strcmp(path, paths[i]) && offsets[i] < 0) { offsets[i] = 0; return i + 16; }
    }
#endif
    int fd = lx_open(path);
    if (fd < 0) errno = ENOENT;
    return fd;
}
int _read(int fd, char *p, int n) {
    if (!n) return 0;
    if (fd == 0) {
        int c = lx_getchar();
        if (c < 0) return 0;
        p[0] = c;
        return 1;
    }
    if (fd >= 3 && fd < 11) return lx_read(fd, p, n);
#ifdef LX_COMPILER
    if (fd < 16 || fd > 17 || offsets[fd-16] < 0) { errno = EBADF; return -1; }
    int i = fd - 16, left = strlen(headers[i]) - offsets[i];
    if (n > left) n = left;
    memcpy(p, headers[i] + offsets[i], n);
    offsets[i] += n;
    return n;
#else
    errno = EBADF; return -1;
#endif
}
int _close(int fd) {
    if (fd >= 3 && fd < 11) return lx_close(fd);
#ifdef LX_COMPILER
    if (fd < 16 || fd > 17) { errno = EBADF; return -1; }
    offsets[fd-16] = -1;
    return 0;
#else
    errno = EBADF; return -1;
#endif
}
#ifdef LX_COMPILER
char *getcwd(char *p, size_t size) {
    if (!p || size < 2) {errno=ERANGE; return NULL;}
    strcpy(p, "/"); return p;
}
char *realpath(const char *name, char *resolved) {
    if (!resolved || (strcmp(name,paths[0]) && strcmp(name,paths[1]))) {errno=ENOENT;return NULL;}
    return strcpy(resolved,name);
}
#endif
