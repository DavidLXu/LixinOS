#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include "../console.h"

#define BASE 0x78000000UL
#define LIMIT 0xa0000000UL /* SVC 37 enforces the capacity of the running kernel. */
/* musl mallocng uses brk for its small-object allocation metadata. */
static size_t heap_capacity(void) {
    static size_t capacity;
    if (!capacity) {
        long reported = console_call(37, ~0UL);
        capacity = reported == (long)(LIMIT - BASE) ? LIMIT - BASE : 128UL << 20;
    }
    return capacity;
}
static size_t brk_capacity(void) {
    return heap_capacity() > (128UL << 20) ? 16UL << 20 : 1UL << 20;
}
#define PAGE 4096
/* The low 128 MiB at SVC 52's base is reserved for compiler metadata. */
#define EXTRA_BASE 0xc8000000UL
#define EXTRA_SIZE (0x100000000UL - EXTRA_BASE)
#define WIDE_BASE (64UL << 30)
#define WIDE_SIZE (4UL << 30)
static unsigned char wide_pages[WIDE_SIZE / PAGE / 8];
static unsigned char extra_pages[EXTRA_SIZE / PAGE / 8];
// The launcher reserves two MiB above the image for the interpreter's C stack.
extern char __end_image[];
static unsigned char pages[(LIMIT - BASE) / PAGE / 8];
static uintptr_t heap_start(void) { return (uintptr_t)__end_image + (2UL << 20); }

static void mark_pages(unsigned char *map, unsigned start, unsigned count, int used) {
    for (unsigned i = start; i < start + count; ++i) {
        unsigned char mask = 1u << (i % 8);
        if (used) map[i / 8] |= mask;
        else map[i / 8] &= ~mask;
    }
}

struct pool {
    unsigned char *map;
    unsigned pages, next, used;
    uintptr_t base, request_offset;
    int svc;
};
static struct pool primary = { pages, sizeof(pages) * 8, 0, 0, BASE, 0, 37 };
static struct pool secondary = { extra_pages, sizeof(extra_pages) * 8, 0, 0, EXTRA_BASE, EXTRA_BASE - 0xc0000000UL, 52 };
static struct pool wide = { wide_pages, sizeof(wide_pages) * 8, 0, 0, WIDE_BASE, 1UL << 40, 37 };

static unsigned find_run(struct pool *p, unsigned first, unsigned end, unsigned count) {
    unsigned run = 0;
    for (unsigned i = first; i < end; ++i) {
        unsigned char byte = p->map[i / 8];
        if (byte == 255) { i |= 7; run = 0; continue; }
        run = byte & (1u << (i % 8)) ? 0 : run + 1;
        if (run == count) return i + 1 - count;
    }
    return p->pages;
}

static long allocate(struct pool *p, unsigned first, unsigned count) {
    if (count > p->pages - first - p->used) return -ENOMEM;
    unsigned hint = p->next > first ? p->next : first;
    unsigned start = find_run(p, hint, p->pages, count);
    if (start == p->pages && hint > first) {
        // Include a run that crosses the hint after adjacent frees.
        unsigned end = hint + count - 1;
        start = find_run(p, first, end < p->pages ? end : p->pages, count);
    }
    if (start == p->pages) return -ENOMEM;
    if (console_call(p->svc, p->request_offset + (start + (uintptr_t)count) * PAGE) < 0)
        return -ENOMEM;
    mark_pages(p->map, start, count, 1);
    p->next = start + count;
    p->used += count;
    void *address = (void *)(p->base + (uintptr_t)start * PAGE);
    memset(address, 0, (size_t)count * PAGE);
    return (long)address;
}
long lx_mmap(long addr, unsigned long bytes, long prot, long flags, long fd) {
    (void)addr;
    if (fd != -1 || (flags & MAP_FIXED_NOREPLACE) ||
        (flags & (MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED)) != (MAP_ANONYMOUS | MAP_PRIVATE) ||
        prot != (PROT_READ | PROT_WRITE))
        return -ENOTSUP;
    if (!bytes || bytes > WIDE_SIZE) return -ENOMEM;
    unsigned count = (bytes + PAGE - 1) / PAGE;
    unsigned first = (heap_start() + brk_capacity() - BASE) / PAGE;
    primary.pages = heap_capacity() / PAGE;
    long address = allocate(&primary, first, count);
    if (address >= 0 || heap_capacity() == (128UL << 20)) return address;
    address = allocate(&secondary, 0, count);
    if (address >= 0) return address;
    return allocate(&wide, 0, count);
}
long lx_munmap(uintptr_t addr, unsigned long size) {
    if (addr % PAGE || !size) return -EINVAL;
    struct pool *p = addr >= WIDE_BASE ? &wide : addr >= EXTRA_BASE ? &secondary : &primary;
    uintptr_t limit = p->base + (uintptr_t)p->pages * PAGE;
    if (addr < p->base || addr >= limit || size > limit - addr ||
        (p == &primary && addr < heap_start() + brk_capacity())) return -EINVAL;
    unsigned start = (addr - p->base) / PAGE, count = (size + PAGE - 1) / PAGE;
    for (unsigned i = start; i < start + count; i++) {
        unsigned mask = 1u << (i % 8);
        if (p->map[i / 8] & mask) {
            p->map[i / 8] &= ~mask;
            --p->used;
        }
    }
    if (start < p->next) p->next = start;
    return 0;
}
long lx_brk(uintptr_t addr) {
    static uintptr_t end;
    if (!end)
        end = heap_start();
    if (addr >= heap_start() && addr <= heap_start() + brk_capacity() &&
        console_call(37, addr - BASE) >= 0)
        end = addr;
    return end;
}
