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
    static size_t arena_used;
    if (increment >= 0) {
        size_t bytes = (size_t)increment;
        if (!arena_used && bytes <= (uintptr_t)__user_heap_limit - (uintptr_t)end) {
            char *previous = end;
            end += bytes;
            return previous;
        }
#if (defined(LX_COMPILER) && !defined(LX_LUST)) || defined(LX_SAVED_RUNTIME)
        // newlib can extend its allocator with a noncontiguous region.
        // Small tools continue to use the space already in their image.
        if (bytes <= 128UL * 1024 * 1024 - arena_used) {
            long base = console_call(37, arena_used + bytes);
            if (base >= 0) {
                void *previous = (char *)base + arena_used;
                arena_used += bytes;
                return previous;
            }
        }
#endif
    }
    errno = ENOMEM;
    return (void *)-1;
}
int _write(int fd, const char *p, int n) {
    if (fd != 1 && fd != 2) { errno=EBADF; return -1; }
    for (int i = 0; i < n; ++i) console_call(fd == 2 ? 11 : 1, (unsigned char)p[i]);
    return n;
}
void lx_abort(void) { finish(134); }
void _exit(int status) { finish(status); }
#if defined(LX_COMPILER) || defined(LX_SAVED_RUNTIME)
static const char *const headers[] = {
    "#define EOF (-1)\n"
    "typedef struct lx_file FILE; FILE *lx_stream(int);\n"
    "#define stdin (lx_stream(0))\n#define stdout (lx_stream(1))\n#define stderr (lx_stream(2))\n"
    "int vsnprintf(char*,unsigned long,const char*,__builtin_va_list);\n"
    "int snprintf(char*,unsigned long,const char*,...);\n"
    "int puts(const char*); int printf(const char*,...); int putchar(int); int getchar(void);\n"
    "int fprintf(FILE*,const char*,...); FILE *fopen(const char*,const char*);\n"
    "unsigned long fread(void*,unsigned long,unsigned long,FILE*); int fclose(FILE*);\n",
#include "lixin-header.inc"
,
#include "gui-header.inc"
,
    "#pragma once\nvoid *memmove(void*,const void*,unsigned long);\n"
    "void *memcpy(void*,const void*,unsigned long);\n"
    "void *memset(void*,int,unsigned long);\n"
    "int memcmp(const void*,const void*,unsigned long);\n"
    "unsigned long strlen(const char*);\n"
    "int strcmp(const char*,const char*); int strncmp(const char*,const char*,unsigned long);\n"
    "char *strcpy(char*,const char*); char *strchr(const char*,int); char *strrchr(const char*,int);\n"
};
static const char *const paths[] = {"/include/stdio.h", "/include/lixin.h", "/include/lixin-gui.h", "/include/string.h"};
static int offsets[] = {-1, -1, -1, -1};
enum { HEADER_COUNT = sizeof(paths) / sizeof(paths[0]), HEADER_FD = 32 };
#endif
int _open(const char *path, int flags, ...) {
    if (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND)) { errno = EROFS; return -1; }
#if defined(LX_COMPILER) || defined(LX_SAVED_RUNTIME)
    for (int i = 0; i < HEADER_COUNT; ++i) {
        if (!strcmp(path, paths[i]) && offsets[i] < 0) { offsets[i] = 0; return i + HEADER_FD; }
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
    if (fd >= 3 && fd < 19) return lx_read(fd, p, n);
#if defined(LX_COMPILER) || defined(LX_SAVED_RUNTIME)
    if (fd < HEADER_FD || fd >= HEADER_FD + HEADER_COUNT || offsets[fd-HEADER_FD] < 0) { errno = EBADF; return -1; }
    int i = fd - HEADER_FD, left = strlen(headers[i]) - offsets[i];
    if (n > left) n = left;
    memcpy(p, headers[i] + offsets[i], n);
    offsets[i] += n;
    return n;
#else
    errno = EBADF; return -1;
#endif
}
int _close(int fd) {
    if (fd >= 3 && fd < 19) return lx_close(fd);
#if defined(LX_COMPILER) || defined(LX_SAVED_RUNTIME)
    if (fd < HEADER_FD || fd >= HEADER_FD + HEADER_COUNT) { errno = EBADF; return -1; }
    offsets[fd-HEADER_FD] = -1;
    return 0;
#else
    errno = EBADF; return -1;
#endif
}
#if defined(LX_COMPILER) || defined(LX_SAVED_RUNTIME)
char *getcwd(char *p, size_t size) {
    if (!p || size < 2) {errno=ERANGE; return NULL;}
    strcpy(p, "/"); return p;
}
char *realpath(const char *name, char *resolved) {
    if (resolved)
        for (int i = 0; i < HEADER_COUNT; ++i)
            if (!strcmp(name, paths[i])) return strcpy(resolved, name);
    errno = ENOENT;
    return NULL;
}
#endif

struct _reent *lx_reent(void) { return _REENT; }
