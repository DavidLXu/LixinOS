#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include "../console.h"

#define BASE 0x78000000UL
#define LIMIT 0x80000000UL
#define PAGE 4096
// The launcher reserves two MiB above the image for the interpreter's C stack.
extern char __end_image[];
static unsigned char pages[(LIMIT - BASE) / PAGE];
static uintptr_t heap_start(void) { return (uintptr_t)__end_image + (2UL << 20); }

long lx_mmap(long addr, unsigned long bytes, long prot, long flags, long fd) {
    // Without MAP_FIXED the requested address is only a placement hint.
    (void)addr;
    if (fd != -1 || (flags & MAP_FIXED_NOREPLACE) ||
        (flags & (MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED)) != (MAP_ANONYMOUS | MAP_PRIVATE) ||
        prot != (PROT_READ | PROT_WRITE))
        return -ENOTSUP;
    if (!bytes || bytes > LIMIT - BASE)
        return -ENOMEM;
    unsigned count = (bytes + PAGE - 1) / PAGE;
    unsigned first = (heap_start() + (1UL << 20) - BASE) / PAGE;
    unsigned run = 0;
    for (unsigned i = first; i < sizeof(pages); i++) {
        run = pages[i] ? 0 : run + 1;
        if (run != count)
            continue;
        if (console_call(37, (i + 1UL) * PAGE) < 0)
            return -ENOMEM;
        unsigned start = i + 1 - count;
        memset(pages + start, 1, count);
        void *p = (void *)(BASE + start * PAGE);
        memset(p, 0, (size_t)count * PAGE);
        return (long)p;
    }
    return -ENOMEM;
}
long lx_munmap(uintptr_t addr, unsigned long size) {
    if (addr < heap_start() + (1UL << 20) || addr >= LIMIT || addr % PAGE || !size ||
        size > LIMIT - addr)
        return -EINVAL;
    memset(pages + (addr - BASE) / PAGE, 0, (size + PAGE - 1) / PAGE);
    return 0;
}
long lx_brk(uintptr_t addr) {
    static uintptr_t end;
    if (!end)
        end = heap_start();
    if (addr >= heap_start() && addr <= heap_start() + (1UL << 20) &&
        console_call(37, addr - BASE) >= 0)
        end = addr;
    return end;
}
