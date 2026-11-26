// Sparse upper Linux address space. Inaccessible allocator cages keep only
// extent records. Writable extents acquire LixinOS pages when first touched.
#include "runtime.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

extern int lx_is_shared_fd(int fd);
extern int lx_kernel_fd(int fd);

#define HIGH_LOW (native_el0 ? (128ULL << 30) : (1ULL << 32))
#define HIGH_LIMIT (native_el0 ? (512ULL << 30) : (1ULL << 47))
#define BLOCK (2ULL << 20)

struct storage {
    unsigned char *data;
    size_t bytes, refs;
    int readonly, file, shared;
};
struct extent {
    u64 lo, hi;
    int prot;
    unsigned char mapped, file_backed, readonly;
    struct storage *storage;
    size_t off;
    struct extent *next;
};
struct upper_memory {
    struct extent *head, *cached;
    unsigned refs;
};
static struct upper_memory initial_upper = {.refs = 1};
static struct upper_memory *upper(void) {
    return guest_process->upper ? guest_process->upper : &initial_upper;
}
#define extents (upper()->head)
#define recent (upper()->cached)

static struct extent **at(u64 va) {
    struct extent **link = &extents;
    while (*link && (*link)->hi <= va)
        link = &(*link)->next;
    return link;
}

static void split(u64 va) {
    struct extent *e = *at(va);
    if (!e || va <= e->lo || va >= e->hi)
        return;
    struct extent *tail = malloc(sizeof *tail);
    if (!tail)
        fail("cannot split virtual memory extent");
    *tail = *e;
    tail->lo = va;
    if (tail->storage) {
        tail->storage->refs++;
        tail->off += va - e->lo;
    }
    e->hi = va;
    e->next = tail;
}

static void drop_storage(struct extent *e) {
    if (e->storage && !--e->storage->refs) {
        munmap(e->storage->data, e->storage->bytes);
        free(e->storage);
    }
    e->storage = NULL;
    e->off = 0;
}

static int covered(u64 lo, u64 end) {
    for (struct extent *e = *at(lo); lo < end; e = e->next) {
        if (!e || e->lo > lo)
            return 0;
        lo = e->hi;
    }
    return 1;
}

static int bounds(u64 lo, u64 bytes) {
    return lo >= HIGH_LOW && lo < HIGH_LIMIT && !(lo % PAGE) && bytes && bytes <= HIGH_LIMIT - lo;
}

static int data_prot(int prot) {
    return prot >= 0 && (prot & (PROT_WRITE | PROT_EXEC)) == PROT_WRITE;
}
static int shared(const struct extent *e) { return e->storage && e->storage->shared; }
u64 high_data_bytes(void) {
    u64 used = 0;
    for (struct extent *e = extents; e; e = e->next)
        if (data_prot(e->prot) && !shared(e))
            used += e->hi - e->lo;
    return used;
}
static u64 overlap(const struct extent *e, u64 lo, u64 hi) {
    return (e->hi < hi ? e->hi : hi) - (e->lo > lo ? e->lo : lo);
}
long high_update(u64 addr, u64 length, int prot) {
    if (!bounds(addr, length))
        return -EINVAL;
    u64 end = (addr + length + PAGE - 1) & ~(u64)(PAGE - 1);
    if (prot != -1 && !covered(addr, end))
        return -ENOMEM;
    u64 growth = 0;
    for (struct extent *e = *at(addr); e && e->lo < end; e = e->next) {
        if (data_prot(prot) && !data_prot(e->prot) && !shared(e))
            growth += overlap(e, addr, end);
        if (prot >= 0 && (prot & PROT_WRITE) &&
            (e->readonly || (e->storage && e->storage->readonly)))
            return -EACCES;
        if (prot == -2 && (e->file_backed || (e->storage && e->storage->file)))
            return -ENOTSUP;
    }
    if (!data_limit_allows(growth))
        return -ENOMEM;
    recent = NULL;
    split(addr);
    split(end);
    struct extent **link = at(addr);
    while (*link && (*link)->lo < end) {
        struct extent *e = *link;
        if (prot == -1) {
            if (native_el0 && e->mapped)
                guest_vm_unmap(guest_space_id(), e->lo, e->hi - e->lo);
            *link = e->next;
            drop_storage(e);
            free(e);
        } else if (prot == -2) {
            if (native_el0) {
                if (e->mapped) {
                    long rc = guest_vm_discard(guest_space_id(), e->lo, e->hi - e->lo);
                    if (rc < 0) return rc;
                }
            } else {
                drop_storage(e); // MADV_DONTNEED for private anonymous pages
            }
            link = &e->next;
        } else {
            if (native_el0) {
                u64 space = guest_space_id();
                if (!e->mapped && prot) {
                    long rc = (e->storage && e->storage->shared)
                                  ? guest_vm_map_shared(space, e->lo, e->hi - e->lo, prot,
                                                        (u64)(uintptr_t)(e->storage->data + e->off))
                                  : guest_vm_map_anon(space, e->lo, e->hi - e->lo, prot);
                    if (rc < 0) return rc;
                    e->mapped = 1;
                } else if (e->mapped) {
                    long rc = guest_vm_protect(space, e->lo, e->hi - e->lo, prot);
                    if (rc < 0) return rc;
                }
            }
            e->prot = prot;
            link = &e->next;
        }
    }
    return 0;
}

static long reserve(u64 hint, u64 length, unsigned prot, unsigned flags, int share, int map_hw) {
    if (!length || length > HIGH_LIMIT - HIGH_LOW || (prot & ~7u) ||
        (flags & MAP_TYPE) != MAP_PRIVATE || !(flags & MAP_ANONYMOUS))
        return -EINVAL;
    u64 size = (length + PAGE - 1) & ~(u64)(PAGE - 1);
    int fixed = flags & (MAP_FIXED | MAP_FIXED_NOREPLACE);
    u64 addr = hint;
    if (!fixed) {
        addr = hint & ~(u64)(PAGE - 1);
        if (!bounds(addr, size))
            addr = HIGH_LOW;
        for (struct extent *e = *at(addr); e && e->lo < addr + size; e = e->next)
            addr = e->hi;
    }
    if (!bounds(addr, size))
        return -ENOMEM;
    struct extent **link = at(addr);
    if ((flags & MAP_FIXED_NOREPLACE) && *link && (*link)->lo < addr + size)
        return -EEXIST;
    if (!share && data_prot(prot) && guest_process->data_soft != UINT64_MAX) {
        u64 growth = size;
        for (struct extent *old = *link; old && old->lo < addr + size; old = old->next)
            if (data_prot(old->prot) && !shared(old))
                growth -= overlap(old, addr, addr + size);
        if (!data_limit_allows(growth))
            return -ENOMEM;
    }
    struct extent *e = calloc(1, sizeof *e);
    if (!e)
        return -ENOMEM;
    if (fixed)
        high_update(addr, size, -1);
    if (native_el0 && map_hw && prot) {
        long rc = guest_vm_map_anon(guest_space_id(), addr, size, (int)prot);
        if (rc < 0) {
            free(e);
            return rc;
        }
        e->mapped = 1;
    }
    link = at(addr);
    e->lo = addr;
    e->hi = addr + size;
    e->prot = (int)prot;
    e->next = *link;
    *link = e;
    recent = e;
    return addr;
}

long high_map(u64 hint, u64 length, unsigned prot, unsigned flags) {
    return reserve(hint, length, prot, flags, 0, 1);
}

void *high_shared_key(u64 addr) {
    if (addr < HIGH_LOW || addr >= HIGH_LIMIT)
        return NULL;
    struct extent *e = recent;
    if (!e || e->lo > addr || e->hi <= addr)
        e = *at(addr);
    if (!e || addr < e->lo || !e->storage || !e->storage->shared)
        return NULL;
    return e->storage->data + e->off + (addr - e->lo);
}

void *high_ptr(u64 addr, u64 len, int access) {
    if (addr < HIGH_LOW || addr >= HIGH_LIMIT || len > HIGH_LIMIT - addr)
        return NULL;
    struct extent *e = recent;
    if (!e || e->lo > addr || e->hi <= addr)
        e = *at(addr);
    if (!e || addr < e->lo || (e->prot & access) != access)
        return NULL;
    u64 end = addr + len;
    if (native_el0) {
        if (end > e->hi) {
            struct extent *prev = e;
            while (prev->hi < end) {
                struct extent *next = prev->next;
                if (!next || next->lo != prev->hi || (next->prot & access) != access)
                    return NULL;
                prev = next;
            }
        }
        recent = e;
        guest_ensure_bound();
        return (void *)(uintptr_t)addr;
    }
    if (!e->storage) {
        u64 lo = addr & ~(BLOCK - 1), hi = (end + BLOCK - 1) & ~(BLOCK - 1);
        if (lo < e->lo)
            lo = e->lo;
        if (hi > e->hi)
            hi = e->hi;
        if (end > hi)
            return NULL;
        split(lo);
        split(hi);
        e = *at(addr);
        struct storage *s = malloc(sizeof *s);
        if (!s)
            fail("cannot allocate sparse backing");
        s->data = mmap(NULL, hi - lo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (s->data == MAP_FAILED)
            fail("cannot commit sparse guest memory");
        s->bytes = hi - lo;
        s->refs = 1;
        s->file = 0;
        s->shared = 0;
        s->readonly = 0;
        e->storage = s;
    }
    if (end > e->hi) {
        struct extent *prev = e;
        while (prev->hi < end) {
            struct extent *next = prev->next;
            if (!next || next->lo != prev->hi || next->storage != e->storage ||
                next->off != prev->off + prev->hi - prev->lo || (next->prot & access) != access)
                return NULL;
            prev = next;
        }
    }
    recent = e;
    return e->storage->data + e->off + (addr - e->lo);
}

// Native file or memfd mappings already have contiguous LixinOS backing. Give
// them a Linux address without requiring it to fit in the low PIE/stack arena.
long high_file(u64 hint, u64 bytes, unsigned prot, unsigned flags, int fd, u64 offset) {
    unsigned type = flags & MAP_TYPE;
    if (!bytes || bytes > HIGH_LIMIT - HIGH_LOW || offset % PAGE || (prot & ~7u) ||
        (type != MAP_SHARED && type != MAP_PRIVATE))
        return -EINVAL;
    if ((flags & MAP_FIXED) && hint < HIGH_LOW)
        return -ENOTSUP;
    size_t rounded = (bytes + PAGE - 1) & ~(u64)(PAGE - 1);
    if (native_el0) {
        u64 space = guest_space_id();
        if (lx_is_shared_fd(fd)) {
            int rights = (type == MAP_SHARED && (prot & PROT_WRITE)) ? (PROT_READ | PROT_WRITE) : PROT_READ;
            void *data = mmap(NULL, bytes, rights, MAP_SHARED, fd, offset);
            if (data == MAP_FAILED)
                return -errno;
            if (type == MAP_SHARED) {
                struct storage *s = malloc(sizeof *s);
                if (!s) {
                    munmap(data, bytes);
                    return -ENOMEM;
                }
                long va = reserve(hint, rounded, prot,
                                  (flags & ~MAP_TYPE) | MAP_PRIVATE | MAP_ANONYMOUS, 1, 0);
                if (va < 0) {
                    free(s);
                    munmap(data, bytes);
                    return va;
                }
                long rc = guest_vm_map_shared(space, (u64)va, rounded, (int)prot, (u64)(uintptr_t)data);
                if (rc < 0) {
                    high_update((u64)va, rounded, -1);
                    free(s);
                    munmap(data, bytes);
                    return rc;
                }
                *s = (struct storage){.data = data,
                                      .bytes = bytes,
                                      .refs = 1,
                                      .readonly = !(rights & PROT_WRITE),
                                      .file = 1,
                                      .shared = 1};
                struct extent *e = *at((u64)va);
                e->storage = s;
                e->mapped = 1;
                e->file_backed = 1;
                e->readonly = (unsigned char)s->readonly;
                return va;
            }
            /* MAP_PRIVATE of a shared memfd: copy into private guest pages. */
            long va = reserve(hint, rounded, prot,
                              (flags & ~MAP_TYPE) | MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
            if (va < 0) {
                munmap(data, bytes);
                return va;
            }
            long rc = guest_vm_map_anon(space, (u64)va, rounded, PROT_READ | PROT_WRITE);
            if (rc >= 0) {
                guest_ensure_bound();
                memcpy((void *)(uintptr_t)va, data, bytes);
                if (prot != (PROT_READ | PROT_WRITE))
                    rc = guest_vm_protect(space, (u64)va, rounded, (int)prot);
            }
            munmap(data, bytes);
            if (rc < 0) {
                high_update((u64)va, rounded, -1);
                return rc;
            }
            struct extent *e = *at((u64)va);
            e->mapped = 1;
            e->file_backed = 1;
            return va;
        }
        /* Regular host file in high space. */
        if (type == MAP_SHARED && (prot & PROT_WRITE))
            return -ENOTSUP;
        if (lx_kernel_fd(fd) < 0)
            return -ENODEV;
        struct stat st;
        if (fstat(fd, &st) < 0)
            return -errno;
        u64 fsz = st.st_size > 0 ? (u64)st.st_size : 0;
        u64 fbytes = offset < fsz ? (bytes < fsz - offset ? bytes : fsz - offset) : 0;
        long va = reserve(hint, rounded, prot,
                          (flags & ~MAP_TYPE) | MAP_PRIVATE | MAP_ANONYMOUS, type == MAP_SHARED, 0);
        if (va < 0)
            return va;
        long rc = guest_vm_map_file(space, (u64)va, rounded, (int)prot, fd, offset, fbytes);
        if (rc < 0) {
            high_update((u64)va, rounded, -1);
            return rc;
        }
        struct extent *e = *at((u64)va);
        e->mapped = 1;
        e->file_backed = 1;
        e->readonly = (type == MAP_SHARED);
        return va;
    }
    int rights = PROT_READ | PROT_WRITE;
    if (type == MAP_SHARED && !(prot & PROT_WRITE))
        rights = PROT_READ;
    void *data = mmap(NULL, bytes, rights, type, fd, offset);
    if (data == MAP_FAILED)
        return -errno;
    struct storage *s = malloc(sizeof *s);
    if (!s) {
        munmap(data, bytes);
        return -ENOMEM;
    }
    long va = reserve(hint, rounded, prot, (flags & ~MAP_TYPE) | MAP_PRIVATE | MAP_ANONYMOUS,
                      type == MAP_SHARED, 0);
    if (va < 0) {
        free(s);
        munmap(data, bytes);
        return va;
    }
    *s = (struct storage){.data = data,
                          .bytes = bytes,
                          .refs = 1,
                          .readonly = !(rights & PROT_WRITE),
                          .file = 1,
                          .shared = type == MAP_SHARED};
    (*at(va))->storage = s;
    return va;
}

int upper_create(struct guest_process *process) {
    process->upper = calloc(1, sizeof *process->upper);
    if (!process->upper)
        return -ENOMEM;
    process->upper->refs = 1;
    return 0;
}
void upper_share(struct guest_process *child) {
    child->upper = upper();
    ++child->upper->refs;
}
void upper_init(void) { guest_process->upper = &initial_upper; }
void upper_destroy(struct guest_process *process) {
    struct upper_memory *m = process->upper;
    process->upper = NULL;
    if (!m || --m->refs)
        return;
    while (m->head) {
        struct extent *e = m->head;
        m->head = e->next;
        drop_storage(e);
        free(e);
    }
    if (m != &initial_upper)
        free(m);
    process->upper = NULL;
}

int upper_clone(struct guest_process *child) {
    struct upper_memory *m = calloc(1, sizeof *m);
    if (!m)
        return -ENOMEM;
    child->upper = m;
    m->refs = 1;
    struct extent **link = &m->head;
    for (struct extent *e = extents; e; e = e->next) {
        struct extent *copy = malloc(sizeof *copy);
        if (!copy)
            return -ENOMEM;
        *copy = *e;
        copy->next = NULL;
        copy->storage = NULL;
        *link = copy;
        link = &copy->next;
        if (!e->storage)
            continue;
        if (native_el0 || e->storage->shared) {
            copy->storage = e->storage;
            ++copy->storage->refs;
            continue;
        }
        // Splitting a reservation preserves the underlying contiguous mapping.
        // Reuse the corresponding copy for each extent of the same storage.
        struct extent *prior = extents, *duplicate = m->head;
        while (prior != e && prior->storage != e->storage) {
            prior = prior->next;
            duplicate = duplicate->next;
        }
        if (prior != e) {
            copy->storage = duplicate->storage;
            ++copy->storage->refs;
            continue;
        }

        struct storage *backing = malloc(sizeof *backing);
        if (!backing)
            return -ENOMEM;
        *backing = *e->storage;
        backing->refs = 1;
        backing->data =
            mmap(NULL, backing->bytes, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (backing->data == MAP_FAILED) {
            free(backing);
            return -ENOMEM;
        }
        memcpy(backing->data, e->storage->data, backing->bytes);
        copy->storage = backing;
    }
    return 0;
}
