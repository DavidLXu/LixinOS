#include "../console.h"
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

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
    for(int i=0;i<n;++i){ if(p[i]=='\n') put_byte('\r'); put_byte(p[i]); }
    return n;
}
void _exit(int status) { finish(status); }
static const char stdio_header[] = "int puts(const char*); int printf(const char*,...); int putchar(int);\n";
static int offset = -1;
int _open(const char *path, int flags, ...) {
    (void)flags;
    if (!strcmp(path,"/include/stdio.h") && offset < 0) {offset=0; return 3;}
    errno=ENOENT; return -1;
}
int _read(int fd, char *p, int n) {
    if(fd!=3 || offset<0){errno=EBADF;return -1;}
    int left=sizeof(stdio_header)-1-offset;
    if(n>left)n=left;
    memcpy(p,stdio_header+offset,n); offset+=n; return n;
}
int _close(int fd){if(fd==3){offset=-1;return 0;} errno=EBADF;return -1;}
char *getcwd(char *p, size_t size) {
    if (!p || size < 2) {errno=ERANGE; return NULL;}
    strcpy(p, "/"); return p;
}
char *realpath(const char *name, char *resolved) {
    if (!resolved || strcmp(name,"/include/stdio.h")) {errno=ENOENT;return NULL;}
    return strcpy(resolved,name);
}
