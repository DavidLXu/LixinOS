#include <stddef.h>
#include <stdint.h>
#include "../console.h"
#include "../tcc/lixin.h"
struct piece {
    const char *path;
    uintptr_t dest;
    unsigned size, checksum;
};
#ifdef LX_APPLICATION
#include "application.h"
#else
#include "layout.h"
#define PHDR_ADDR (IMAGE_BASE + 64)
#endif
#ifndef LX_ARGV0
#define LX_ARGV0 "/bin/python3"
#endif

static void fail(const char *message) {
    lx_error(message);
    finish(1);
}

// Form the initial AArch64 ELF stack for musl crt1, including TLS program headers.
__attribute__((section(".text.start"), noreturn)) void _start(const char *cwd, const char *args,
                                                              size_t length) {
    (void)cwd;
    if (console_call(37, STACK_TOP - IMAGE_BASE) != IMAGE_BASE)
        fail(LX_ARGV0 ": insufficient memory\n");
    uintptr_t top = STACK_TOP;
    if (length > 32768)
        fail(LX_ARGV0 ": arguments too long\n");
    char *strings = (char *)(top - length);
    for (size_t i = 0; i < length; i++)
        strings[i] = args[i];

#ifndef LX_APPLICATION
    for (unsigned i = 0; i < sizeof(pieces) / sizeof(pieces[0]); i++) {
        const struct piece *p = &pieces[i];
        int fd = lx_open(p->path);
        uint32_t hash = 2166136261U;
        unsigned offset = 0;
        while (fd >= 0 && offset < p->size) {
            unsigned char *out = (unsigned char *)p->dest + offset;
            int n = lx_read(fd, out, p->size - offset);
            if (n <= 0)
                break;
            for (int j = 0; j < n; j++)
                hash = (hash ^ out[j]) * 16777619U;
            offset += n;
        }
        if (fd >= 0)
            lx_close(fd);
        if (offset != p->size || hash != p->checksum)
            fail(LX_ARGV0 ": missing or damaged program files\n");
    }
    if (console_call(46, TEXT_BYTES))
        fail(LX_ARGV0 ": kernel lacks executable image support\n");
#endif
    uintptr_t random = ((uintptr_t)strings & ~(uintptr_t)15) - 16;
    if (lx_random((void *)random, 16))
        fail(LX_ARGV0 ": entropy unavailable\n");
    uintptr_t *sp = (uintptr_t *)(random - (512 + 32) * sizeof(uintptr_t));
    uintptr_t *v = sp + 1;
    *v++ = (uintptr_t)LX_ARGV0;
#ifdef LX_PIP
    *v++ = (uintptr_t)"-m";
    *v++ = (uintptr_t)"pip";
#endif
    for (size_t i = 0; i < length;) {
        if (v - sp >= 513)
            fail(LX_ARGV0 ": too many arguments\n");
        *v++ = (uintptr_t)&strings[i];
        while (i < length && strings[i])
            i++;
        if (i++ == length)
            fail(LX_ARGV0 ": invalid argument\n");
    }
    *sp = v - sp - 1;
    *v++ = 0;
#ifdef LX_GCC
    *v++ = (uintptr_t)"COMPILER_PATH=/lib/gcc/bin:/bin";
    *v++ = (uintptr_t)"PATH=/bin";
    *v++ = (uintptr_t)"TMPDIR=/tmp";
#elif !defined(LX_APPLICATION)
    *v++ = (uintptr_t)"PYTHON_BASIC_REPL=1";
    *v++ = (uintptr_t)"_PYTHON_SYSCONFIGDATA_NAME=_lixin_sysconfig";
    *v++ = (uintptr_t)"PIP_DISABLE_PIP_VERSION_CHECK=1";
    *v++ = (uintptr_t)"PIP_PROGRESS_BAR=off";
    *v++ = (uintptr_t)"PIP_ONLY_BINARY=:all:";
    *v++ = (uintptr_t)"PIP_NO_CACHE_DIR=1";
    *v++ = (uintptr_t)"PIP_NO_COMPILE=1";
    *v++ = (uintptr_t)"PIP_ROOT_USER_ACTION=ignore";
    *v++ = (uintptr_t)"SSL_CERT_FILE=/lib/cpython-3.14.5/roots.pem";
#endif
    *v++ = (uintptr_t)"LC_ALL=C.UTF-8";
    *v++ = 0;
    *v++ = 3;
    *v++ = (uintptr_t)PHDR_ADDR; // AT_PHDR
    *v++ = 4;
    *v++ = 56;
    *v++ = 5;
    *v++ = PHNUM;
    *v++ = 6;
    *v++ = 4096;
    *v++ = 25;
    *v++ = random;
    *v++ = 0;
    *v++ = 0;
    asm volatile("mov sp,%0\nbr %1" ::"r"(sp), "r"((uintptr_t)ENTRY_PC) : "memory");
    __builtin_unreachable();
}
