#include "mmu.h"
#include "runtime.h"
#include "../console.h"
#define LX_USER_GUEST_ONLY
#include "../../Kernel/native/guest.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>

extern int lx_kernel_fd(int fd);
int native_el0;
static u64 bound_space;

static long guest_svc(u64 op, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
    u64 req[8] = {op, a1, a2, a3, a4, a5, a6, a7};
    return console_call(LX_GUEST_SVC_NR, (long)req);
}
void guest_vm_probe(void) {
    if (getenv("LIXINOS_INTERP")) {
        native_el0 = 0;
        return;
    }
    native_el0 = guest_svc(LX_GUEST_QUERY, 0, 0, 0, 0, 0, 0, 0) == LX_GUEST_ABI_VER;
}
long guest_vm_map_anon(u64 space, u64 va, u64 len, int prot) {
    return guest_svc(LX_GUEST_MAP_ANON, space, va, len, (u64)(unsigned)prot, 0, 0, 0);
}
long guest_vm_map_file(u64 space, u64 va, u64 len, int prot, int host_fd, u64 off, u64 file_bytes) {
    int kfd = lx_kernel_fd(host_fd);
    if (kfd < 0) return -EBADF;
    return guest_svc(LX_GUEST_MAP_FILE, space, va, len, (u64)(unsigned)prot, (u64)kfd, off, file_bytes);
}
long guest_vm_map_shared(u64 space, u64 va, u64 len, int prot, u64 shared_va) {
    return guest_svc(LX_GUEST_MAP_SHARED, space, va, len, (u64)(unsigned)prot, shared_va, 0, 0);
}
long guest_vm_protect(u64 space, u64 va, u64 len, int prot) {
    return guest_svc(LX_GUEST_PROTECT, space, va, len, (u64)(unsigned)prot, 0, 0, 0);
}
long guest_vm_unmap(u64 space, u64 va, u64 len) {
    return guest_svc(LX_GUEST_UNMAP, space, va, len, 0, 0, 0, 0);
}
long guest_vm_discard(u64 space, u64 va, u64 len) {
    return guest_svc(LX_GUEST_DISCARD, space, va, len, 0, 0, 0, 0);
}
long guest_vm_run(u64 space, void *ctx, u64 slice_ticks) {
    bound_space = space;
    return guest_svc(LX_GUEST_RUN, space, (u64)(uintptr_t)ctx, slice_ticks, 0, 0, 0, 0);
}

/* Read-only file segment cache: shares physical pages across execve/dlopen. */
enum { RO_CACHE_MAX = 256 };
struct ro_cache_entry {
    u64 dev, inode, fsize, off, len, file_bytes, cache_va;
    struct timespec mtime;
    int prot;
};
static u64 cache_space;
static u64 cache_cursor = LX_GUEST_HIGH_BASE;
static struct ro_cache_entry ro_cache[RO_CACHE_MAX];
static unsigned ro_cache_count;

static int map_ro_cached(u64 dst_space, u64 dst_va, u64 map_len, int prot,
                         struct backing *f, u64 off, u64 file_bytes) {
    for (unsigned i = 0; i < ro_cache_count; ++i) {
        struct ro_cache_entry *e = &ro_cache[i];
        if (e->dev == f->dev && e->inode == f->inode && e->fsize == f->size &&
            e->mtime.tv_sec == f->modified.tv_sec && e->mtime.tv_nsec == f->modified.tv_nsec &&
            e->off == off && e->len == map_len && e->file_bytes == file_bytes && e->prot == prot) {
            return guest_svc(LX_GUEST_MAP_ALIAS, dst_space, dst_va, map_len,
                             (u64)(unsigned)prot, cache_space, e->cache_va, 0) == 0;
        }
    }
    if (ro_cache_count >= RO_CACHE_MAX || map_len > LX_GUEST_HIGH_END - cache_cursor)
        return 0;
    if (!cache_space) {
        long id = guest_svc(LX_GUEST_SPACE_CREATE, 0, 0, 0, 0, 0, 0, 0);
        if (id <= 0) return 0;
        cache_space = (u64)id;
    }
    u64 cva = cache_cursor;
    if (guest_vm_map_file(cache_space, cva, map_len, prot, f->fd, off, file_bytes) < 0)
        return 0;
    cache_cursor += map_len;
    ro_cache[ro_cache_count++] = (struct ro_cache_entry){
        .dev = f->dev, .inode = f->inode, .fsize = f->size, .off = off,
        .len = map_len, .file_bytes = file_bytes, .cache_va = cva,
        .mtime = f->modified, .prot = prot,
    };
    return guest_svc(LX_GUEST_MAP_ALIAS, dst_space, dst_va, map_len,
                     (u64)(unsigned)prot, cache_space, cva, 0) == 0;
}

enum { PRESENT = 8, RESIDENT = 16, BEYOND_EOF = 32, SHARED_READONLY = 64 };
struct page {
    struct backing *file;
    u64 offset;
    unsigned short bytes;
    unsigned char flags;
};
struct guest_memory {
    unsigned char *arena;
    struct page *table;
    u64 space_id;
    unsigned char *nflags;
    u64 cursor, brk, data_bytes;
    unsigned refs;
};
#define ram (guest_process->memory->arena)
#define pages (guest_process->memory->table)
#define nflags_tbl (guest_process->memory->nflags)
#define next_map (guest_process->memory->cursor)
static inline size_t nidx(u64 addr) { return (addr - MAIN_BASE) / PAGE; }
static inline size_t ncount(void) { return (ARENA - MAIN_BASE) / PAGE; }
u64 guest_space_id(void) { return guest_process->memory->space_id; }
void guest_ensure_bound(void) {
    u64 id = guest_process->memory->space_id;
    if (bound_space != id) {
        if (guest_svc(LX_GUEST_BIND, id, 0, 0, 0, 0, 0, 0) < 0)
            fail("cannot bind guest address space");
        bound_space = id;
    }
}
void *guest_futex_key(u64 addr, struct guest_memory **owner_out) {
    if (!native_el0) {
        *owner_out = NULL;
        return guest_ptr(addr, 4, PROT_READ);
    }
    if (addr >= ARENA) {
        void *sk = high_shared_key(addr);
        if (sk) {
            *owner_out = NULL;
            return sk;
        }
    }
    *owner_out = guest_process->memory;
    return (void *)(uintptr_t)addr;
}
static unsigned long file_faults, file_bytes_read;
static unsigned live_files, native_limit = 35;
static struct backing *all_files;
static int make_fd_available(void);

static void stats(void) {
    fprintf(stderr, "linux-run: paged in %lu file pages (%lu bytes)\n", file_faults,
            file_bytes_read);
}
struct backing *open_backing(const char *path) {
    if (live_files + 11 >= native_limit)
        make_fd_available();
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        errno = ENOENT;
        return NULL;
    }
    struct backing *f = malloc(sizeof *f);
    if (!f) {
        close(fd);
        errno = ENOMEM;
        return NULL;
    }
    *f = (struct backing){.fd = fd, .refs = 1, .open = 1, .size = st.st_size};
    f->inode = st.st_ino;
    f->dev = st.st_dev;
    f->modified = st.st_mtim;
    f->path = strdup(path);
    if (!f->path) {
        close(fd);
        free(f);
        errno = ENOMEM;
        return NULL;
    }
    f->next = all_files;
    all_files = f;
    ++live_files;
    return f;
}
static void release_backing(struct backing *f) {
    if (f && --f->refs == 0) {
        struct backing **link = &all_files;
        while (*link != f)
            link = &(*link)->next;
        *link = f->next;
        close(f->fd);
        --live_files;
        free(f->path);
        free(f);
    }
}
void close_backing(struct backing *f) {
    --f->open;
    release_backing(f);
}
int memory_create(struct guest_process *process) {
    struct guest_memory *m = calloc(1, sizeof *m);
    if (!m)
        return -ENOMEM;
    process->memory = m;
    m->cursor = MMAP_BASE;
    m->brk = BRK_BASE;
    m->refs = 1;
    if (native_el0) {
        long id = guest_svc(LX_GUEST_SPACE_CREATE, 0, 0, 0, 0, 0, 0, 0);
        if (id <= 0) {
            memory_destroy(process);
            return id < 0 ? (int)id : -ENOMEM;
        }
        m->space_id = (u64)id;
        m->nflags = calloc(ncount(), 1);
        if (!m->nflags) {
            memory_destroy(process);
            return -ENOMEM;
        }
        return 0;
    }
    m->arena = mmap(NULL, ARENA, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    m->table = calloc(ARENA / PAGE, sizeof *m->table);
    if (m->arena == MAP_FAILED || !m->table) {
        memory_destroy(process);
        return -ENOMEM;
    }
    return 0;
}
u64 *memory_brk(void) { return &guest_process->memory->brk; }
void memory_share(struct guest_process *child) {
    child->memory = guest_process->memory;
    ++child->memory->refs;
}
void mem_init(void) {
    struct rlimit limit;
    if (!getrlimit(RLIMIT_NOFILE, &limit) && limit.rlim_cur >= 12 && limit.rlim_cur <= GUEST_FDS)
        native_limit = limit.rlim_cur;
    if (memory_create(guest_process))
        fail("cannot reserve guest virtual address space");
    if (tracing)
        atexit(stats);
}
// Duplicate the page metadata and resident contents. File pages that have
// not faulted in yet retain their original immutable backing description.
void memory_destroy(struct guest_process *process) {
    struct guest_memory *m = process->memory;
    process->memory = NULL;
    if (!m || --m->refs)
        return;
    if (m->space_id) {
        if (bound_space == m->space_id)
            bound_space = 0;
        guest_svc(LX_GUEST_SPACE_DESTROY, m->space_id, 0, 0, 0, 0, 0, 0);
    }
    free(m->nflags);
    if (m->table) {
        for (size_t i = 0; i < ARENA / PAGE; ++i)
            release_backing(m->table[i].file);
        free(m->table);
    }
    if (m->arena && m->arena != MAP_FAILED)
        munmap(m->arena, ARENA);
    free(m);
    process->memory = NULL;
}
int memory_clone(struct guest_process *child) {
    if (native_el0) {
        struct guest_memory *m = calloc(1, sizeof *m);
        if (!m)
            return -ENOMEM;
        child->memory = m;
        m->cursor = next_map;
        m->data_bytes = guest_process->memory->data_bytes;
        m->brk = guest_process->memory->brk;
        m->refs = 1;
        long id = guest_svc(LX_GUEST_SPACE_CLONE, guest_process->memory->space_id, 0, 0, 0, 0, 0, 0);
        if (id <= 0) {
            memory_destroy(child);
            return id < 0 ? (int)id : -ENOMEM;
        }
        m->space_id = (u64)id;
        m->nflags = malloc(ncount());
        if (!m->nflags) {
            memory_destroy(child);
            return -ENOMEM;
        }
        memcpy(m->nflags, nflags_tbl, ncount());
        return 0;
    }
    int rc = memory_create(child);
    if (rc)
        return rc;
    struct guest_memory *m = child->memory;
    m->cursor = next_map;
    m->data_bytes = guest_process->memory->data_bytes;
    m->brk = guest_process->memory->brk;
    for (size_t i = 0; i < ARENA / PAGE;) {
        if (!(pages[i].flags & RESIDENT)) {
            m->table[i] = pages[i];
            if (pages[i].file)
                ++pages[i].file->refs;
            ++i;
            continue;
        }
        size_t start = i;
        while (i < ARENA / PAGE && (pages[i].flags & RESIDENT))
            ++i;
        size_t bytes = (i - start) * PAGE;
        if (mprotect(m->arena + start * PAGE, bytes, PROT_READ | PROT_WRITE))
            return -errno;
        memcpy(m->arena + start * PAGE, ram + start * PAGE, bytes);
        memcpy(m->table + start, pages + start, (i - start) * sizeof *pages);
    }
    return 0;
}

static void page_in(size_t i) {
    struct page *p = pages + i;
    if (p->flags & BEYOND_EOF)
        fail("access past end of mapped file");
    unsigned char *dst = ram + i * PAGE;
    if (mprotect(dst, PAGE, PROT_READ | PROT_WRITE))
        fail("cannot commit guest page");
    memset(dst, 0, PAGE);
    if (p->file) {
        if (read_segment(p->file->fd, dst, p->bytes, p->offset))
            fail("short page read");
        file_faults++;
        file_bytes_read += p->bytes;
        release_backing(p->file);
        p->file = NULL;
    }
    p->flags |= RESIDENT;
}
// Before the native descriptor table fills up, finish reading
// the smallest closed mapping so its fd can be reused without reopening its path.
static int make_fd_available(void) {
    if (native_el0)
        return 0;
    struct backing *smallest = NULL;
    for (size_t i = 0; i < ARENA / PAGE; i++) {
        struct backing *f = pages[i].file;
        if (f && !f->open && (!smallest || f->refs < smallest->refs))
            smallest = f;
    }
    if (!smallest)
        return 0;
    smallest->refs++;
    for (size_t i = 0; i < ARENA / PAGE; i++)
        if (pages[i].file == smallest)
            page_in(i);
    release_backing(smallest);
    return 1;
}
void reserve_descriptors(unsigned used) {
    while (used + live_files + 7 > native_limit && make_fd_available()) {
    }
}
void *guest_ptr(u64 addr, u64 len, int access) {
    if (addr >= ARENA)
        return high_ptr(addr, len, access);
    if (addr < MAIN_BASE || addr >= ARENA || len > ARENA - addr)
        return NULL;
    if (native_el0) {
        if (len) {
            const unsigned required = PRESENT | access;
            unsigned char *nf = nflags_tbl;
            size_t first = nidx(addr), last = nidx(addr + len - 1);
            for (size_t i = first; i <= last; i++) {
                if ((nf[i] & required) != required || (nf[i] & BEYOND_EOF))
                    return NULL;
            }
        }
        guest_ensure_bound();
        return (void *)(uintptr_t)addr;
    }
    if (len) {
        const unsigned required = PRESENT | access;
        for (size_t i = addr / PAGE; i <= (addr + len - 1) / PAGE; i++) {
            if ((pages[i].flags & required) != required)
                return NULL;
            if (!(pages[i].flags & RESIDENT))
                page_in(i);
        }
    }
    return ram + addr;
}
static int valid_range(u64 start, u64 end) {
    return start >= MAIN_BASE && start % PAGE == 0 && end > start && end <= ARENA;
}
static void forget(size_t i) {
    struct page *p = pages + i;
    if (p->flags & RESIDENT) {
        // Keep the virtual reservation; drop its old contents and physical pages.
        void *dst = ram + i * PAGE;
        if (mmap(dst, PAGE, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) != dst)
            fail("cannot discard guest page");
    }
    release_backing(p->file);
    *p = (struct page){0};
}
int data_limit_allows(u64 growth) {
    if (!growth || guest_process->data_soft == UINT64_MAX)
        return 1;
    u64 used = guest_process->memory->data_bytes + high_data_bytes();
    return used <= guest_process->data_soft && growth <= guest_process->data_soft - used;
}
// Charge private writable non-executable mappings, excluding the program stack.
static int account_pages(u64 start, u64 end, int prot) {
    if (end > STACK_BASE)
        end = STACK_BASE;
    long delta = 0;
    int writable = prot >= 0 && (prot & (PROT_WRITE | PROT_EXEC)) == PROT_WRITE;
    for (u64 pos = start; pos < end; pos += PAGE) {
        int old = native_el0 ? nflags_tbl[nidx(pos)] : pages[pos / PAGE].flags;
        delta += PAGE * (long)(writable - ((old & (PRESENT | PROT_WRITE | PROT_EXEC |
                                                   SHARED_READONLY)) == (PRESENT | PROT_WRITE)));
    }
    if (delta > 0 && !data_limit_allows(delta))
        return -ENOMEM;
    guest_process->memory->data_bytes += delta;
    return 0;
}
// Each open still owns its file position. Lazy pages use pread and may share
// the same immutable library backing across the browser, zygotes and renderer.
static struct backing *mapping_backing(struct backing *f) {
    if (!f)
        return NULL;
    struct backing *best = f;
    for (struct backing *item = all_files; item; item = item->next)
        if (item != f && item->refs > item->open && item->refs > best->refs &&
            item->dev == f->dev && item->inode == f->inode && item->size == f->size &&
            item->modified.tv_sec == f->modified.tv_sec &&
            item->modified.tv_nsec == f->modified.tv_nsec && !strcmp(item->path, f->path))
            best = item;
    return best;
}
int set_mapping(u64 start, u64 length, int prot, struct backing *f, u64 off, u64 file_bytes) {
    if (length > ARENA || !valid_range(start, start + length) || (prot & ~7) ||
        off > INT64_MAX - ARENA || file_bytes > length)
        return -EINVAL;
    u64 map_len = (length + PAGE - 1) & ~(u64)(PAGE - 1);
    if (start + map_len > ARENA)
        return -EINVAL;
    if (account_pages(start, start + map_len, prot))
        return -ENOMEM;
    if (native_el0) {
        u64 space = guest_space_id();
        if (tracing && f && (prot & PROT_EXEC))
            fprintf(stderr, "linux-run: code %s va=%#lx offset=%#lx size=%#lx\n", f->path,
                    (unsigned long)start, (unsigned long)off, (unsigned long)length);
        long rc = 0;
        if (!f || !file_bytes) {
            rc = guest_vm_map_anon(space, start, map_len, prot);
        } else {
            u64 valid_file = off < f->size ? (file_bytes < f->size - off ? file_bytes : f->size - off) : 0;
            u64 valid_pages = (valid_file + PAGE - 1) & ~(u64)(PAGE - 1);
            u64 req_pages = (file_bytes + PAGE - 1) & ~(u64)(PAGE - 1);
            if (valid_pages > map_len) valid_pages = map_len;
            if (req_pages > map_len) req_pages = map_len;
            if (valid_pages) {
                if (!(prot & PROT_WRITE) && prot && valid_pages == map_len &&
                    map_ro_cached(space, start, valid_pages, prot, f, off, valid_file)) {
                    rc = 0;
                } else {
                    rc = guest_vm_map_file(space, start, valid_pages, prot, f->fd, off, valid_file);
                }
            }
            if (!rc && req_pages > valid_pages)
                rc = guest_vm_unmap(space, start + valid_pages, req_pages - valid_pages);
            if (!rc && map_len > req_pages)
                rc = guest_vm_map_anon(space, start + req_pages, map_len - req_pages, prot);
        }
        if (rc < 0)
            return (int)rc;
        unsigned char *nf = nflags_tbl;
        for (u64 pos = 0; pos < map_len; pos += PAGE) {
            unsigned char fl = PRESENT | (unsigned char)prot;
            if (f && pos < file_bytes) {
                if (off + pos >= f->size)
                    fl |= BEYOND_EOF;
                else
                    fl |= RESIDENT; /* marks file-backed page for madvise check */
            }
            nf[nidx(start + pos)] = fl;
        }
        return 0;
    }
    struct backing *mapped_file = mapping_backing(f);
    if (mapped_file)
        ++mapped_file->refs;
    if (tracing && f && (prot & PROT_EXEC))
        fprintf(stderr, "linux-run: code %s va=%#lx offset=%#lx size=%#lx\n", f->path,
                (unsigned long)start, (unsigned long)off, (unsigned long)length);
    for (u64 pos = 0; pos < length; pos += PAGE) {
        size_t i = (start + pos) / PAGE;
        forget(i);
        pages[i].flags = PRESENT | prot;
        if (f && pos < file_bytes) {
            if (off + pos >= f->size) {
                pages[i].flags |= BEYOND_EOF;
                continue;
            }
            pages[i].file = mapped_file;
            pages[i].offset = off + pos;
            u64 bytes = file_bytes - pos;
            if (bytes > f->size - off - pos)
                bytes = f->size - off - pos;
            pages[i].bytes = bytes > PAGE ? PAGE : bytes;
            mapped_file->refs++;
        }
    }
    release_backing(mapped_file);
    return 0;
}
long discard_pages(u64 start, u64 length) {
    if (start < MAIN_BASE || length > ARENA - start)
        return -ENOMEM;
    u64 end = (start + length + PAGE - 1) & ~(u64)(PAGE - 1);
    if (native_el0) {
        unsigned char *nf = nflags_tbl;
        for (u64 pos = start; pos < end; pos += PAGE) {
            unsigned char fl = nf[nidx(pos)];
            if (!(fl & PRESENT))
                return -ENOMEM;
            if (fl & (RESIDENT | BEYOND_EOF | SHARED_READONLY))
                return -ENOTSUP;
        }
        return guest_vm_discard(guest_space_id(), start, end - start);
    }
    for (size_t i = start / PAGE; i < end / PAGE; ++i) {
        if (!(pages[i].flags & PRESENT))
            return -ENOMEM;
        // File pages keep their byte count even after page_in closes the backing.
        if (pages[i].bytes || (pages[i].flags & (BEYOND_EOF | SHARED_READONLY)))
            return -ENOTSUP;
    }
    // The native arena is private anonymous memory. Its VM keeps permissions
    // and supplies zero pages on the next access, including after fork.
    return madvise(ram + start, end - start, MADV_DONTNEED) < 0 ? -errno : 0;
}
int page_update(u64 start, u64 end, int prot, int require_mapped) {
    if (!valid_range(start, end) || (prot >= 0 && (prot & ~7)))
        return -EINVAL;
    u64 map_end = (end + PAGE - 1) & ~(u64)(PAGE - 1);
    if (map_end > ARENA)
        return -EINVAL;
    if (require_mapped) {
        for (u64 pos = start; pos < map_end; pos += PAGE) {
            unsigned char fl = native_el0 ? nflags_tbl[nidx(pos)] : pages[pos / PAGE].flags;
            if (!(fl & PRESENT))
                return -ENOMEM;
            if (prot >= 0 && (prot & PROT_WRITE) && (fl & SHARED_READONLY))
                return -EACCES;
        }
    }
    if (account_pages(start, map_end, prot))
        return -ENOMEM;
    if (native_el0) {
        u64 space = guest_space_id();
        unsigned char *nf = nflags_tbl;
        if (prot < 0) {
            long rc = guest_vm_unmap(space, start, map_end - start);
            if (rc < 0) return (int)rc;
            memset(nf + nidx(start), 0, (map_end - start) / PAGE);
        } else {
            for (u64 pos = start; pos < map_end;) {
                unsigned char fl = nf[nidx(pos)];
                int mapped = !!(fl & PRESENT);
                int eof = !!(fl & BEYOND_EOF);
                u64 run = pos;
                while (pos < map_end && !!(nf[nidx(pos)] & PRESENT) == mapped &&
                       !!(nf[nidx(pos)] & BEYOND_EOF) == eof) {
                    pos += PAGE;
                }
                if (!eof) {
                    long rc = mapped ? guest_vm_protect(space, run, pos - run, prot)
                                     : guest_vm_map_anon(space, run, pos - run, prot);
                    if (rc < 0) return (int)rc;
                }
                for (u64 p = run; p < pos; p += PAGE) {
                    size_t idx = nidx(p);
                    nf[idx] = PRESENT | (unsigned char)prot |
                              (nf[idx] & (RESIDENT | BEYOND_EOF | SHARED_READONLY));
                }
            }
        }
        if (prot < 0 && start >= MMAP_BASE && start < next_map)
            next_map = start;
        return 0;
    }
    for (size_t i = start / PAGE; i <= (end - 1) / PAGE; i++) {
        if (prot < 0)
            forget(i);
        else
            pages[i].flags =
                PRESENT | prot | (pages[i].flags & (RESIDENT | BEYOND_EOF | SHARED_READONLY));
    }
    if (prot < 0 && start >= MMAP_BASE && start < next_map)
        next_map = start;
    return 0;
}

int read_segment(int fd, void *out, size_t len, u64 offset) {
    while (len) {
        ssize_t n = pread(fd, out, len, offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        out = (unsigned char *)out + n;
        offset += n;
        len -= n;
    }
    return 0;
}

u64 map_linux(u64 addr, u64 length, unsigned prot, unsigned flags, struct backing *file, u64 off) {
    if (addr >= ARENA && file && !(flags & MAP_ANONYMOUS))
        return high_file(addr, length, prot, flags, file->fd, off);
    if ((addr >= ARENA || length > STACK_BASE - MMAP_BASE) && (flags & MAP_ANONYMOUS))
        return high_map(addr, length, prot, flags);
    int shared_file = (flags & MAP_TYPE) == MAP_SHARED && file && !(flags & MAP_ANONYMOUS);
    if (!length || length > ARENA || (off & (PAGE - 1)) || off > INT64_MAX - ARENA ||
        ((flags & MAP_TYPE) != MAP_PRIVATE && !shared_file) || (prot & ~7u))
        return -EINVAL;
    if (shared_file && (prot & PROT_WRITE))
        return -EACCES; // File-backed descriptors in this path are opened read-only.
    u64 size = (length + PAGE - 1) & ~(u64)(PAGE - 1);
    if (!(flags & MAP_FIXED)) {
        addr = next_map;
        while (addr < STACK_BASE && size <= STACK_BASE - addr) {
            size_t p;
            if (native_el0) {
                unsigned char *nf = nflags_tbl;
                size_t base = nidx(addr);
                for (p = 0; p < size / PAGE && !(nf[base + p] & PRESENT); p++) {
                }
            } else {
                for (p = 0; p < size / PAGE && !(pages[addr / PAGE + p].flags & PRESENT); p++) {
                }
            }
            if (p == size / PAGE)
                break;
            addr += (p + 1) * PAGE;
        }
    }
    if (addr < MMAP_BASE || addr > STACK_BASE || size > STACK_BASE - addr || addr % PAGE) {
        if (!(flags & MAP_FIXED) && (flags & MAP_ANONYMOUS))
            return high_map(0, length, prot, flags);
        return -ENOMEM;
    }
    int rc = set_mapping(addr, size, prot, (flags & MAP_ANONYMOUS) ? NULL : file, off, size);
    if (rc < 0)
        return rc;
    if (shared_file) {
        if (native_el0) {
            for (u64 pos = addr; pos < addr + size; pos += PAGE)
                nflags_tbl[nidx(pos)] |= SHARED_READONLY;
        } else {
            for (size_t i = addr / PAGE; i < (addr + size) / PAGE; ++i)
                pages[i].flags |= SHARED_READONLY;
        }
    }
    if (!(flags & MAP_FIXED))
        next_map = addr + size;
    return addr;
}

void *mem_host_ptr(CPU *cpu, u64 addr, unsigned size, AccType type) {
    if (!size)
        return NULL;
    (void)cpu;
    return guest_ptr(addr, size,
                     type == ACC_WRITE  ? PROT_WRITE
                     : type == ACC_EXEC ? PROT_EXEC
                                        : PROT_READ);
}
// CPU operands and small syscall records can straddle unrelated backing pages.
int guest_copy(u64 addr, void *value, size_t size, int write) {
    if (!size)
        return 1;
    if (size > PAGE || addr > UINT64_MAX - size)
        return 0;
    size_t first = PAGE - addr % PAGE;
    if (first > size)
        first = size;
    int prot = write ? PROT_WRITE : PROT_READ;
    void *p = guest_ptr(addr, first, prot);
    void *q = first < size ? guest_ptr(addr + first, size - first, prot) : NULL;
    if (!p || (first < size && !q))
        return 0;
    if (write) {
        memcpy(p, value, first);
        if (q)
            memcpy(q, (unsigned char *)value + first, size - first);
    } else {
        memcpy(value, p, first);
        if (q)
            memcpy((unsigned char *)value + first, q, size - first);
    }
    return 1;
}
// During one interpreter slice, mappings and permissions stay stable. Cache
// read and write translations separately so a read cannot authorize a write.
static struct data_translation {
    u64 page, generation;
    unsigned char *host;
} data_cache[2][128];
static u64 translation_generation = 1;

static void scalar_copy(u64 addr, void *value, size_t size, int write) {
    size_t offset = addr % PAGE;
    if (size <= PAGE - offset) {
        u64 page = addr / PAGE;
        struct data_translation *t = &data_cache[write][page % 128];
        if (t->generation != translation_generation || t->page != page) {
            t->host = guest_ptr(addr - offset, PAGE, write ? PROT_WRITE : PROT_READ);
            t->page = page;
            t->generation = translation_generation;
        }
        if (t->host) {
            void *native = t->host + offset;
            if (write)
                memcpy(native, value, size);
            else
                memcpy(value, native, size);
            return;
        }
    }
    if (!guest_copy(addr, value, size, write)) {
        if (tracing)
            fprintf(stderr, "linux-run: access addr=%#lx bytes=%zu prot=%d\n", (unsigned long)addr,
                    size, write ? PROT_WRITE : PROT_READ);
        fail("invalid guest memory access");
    }
}
bool mem_read(CPU *cpu, u64 addr, unsigned size, u64 *out) {
    (void)cpu;
    if (size > sizeof *out)
        fail("invalid scalar read");
    *out = 0;
    scalar_copy(addr, out, size, 0);
    return true;
}
bool mem_write(CPU *cpu, u64 addr, unsigned size, u64 value) {
    (void)cpu;
    if (size > sizeof value)
        fail("invalid scalar write");
    scalar_copy(addr, &value, size, 1);
    return true;
}
bool mem_read128(CPU *cpu, u64 addr, V128 *out) {
    (void)cpu;
    scalar_copy(addr, out, 16, 0);
    return true;
}
bool mem_write128(CPU *cpu, u64 addr, const V128 *in) {
    (void)cpu;
    scalar_copy(addr, (void *)in, 16, 1);
    return true;
}
// Only one interpreted CPU executes instructions at a time under the CPU lock.
void casp16_mutex_lock(void) {}
void casp16_mutex_unlock(void) {}
FetchCache g_fcache;
bool mem_ifetch_slow(CPU *cpu, u64 addr, u32 *insn) {
    (void)cpu;
    u64 page = addr & ~(u64)(PAGE - 1);
    if (addr % sizeof *insn || !(g_fcache.host = guest_ptr(page, PAGE, PROT_EXEC)))
        fail("invalid instruction address");
    g_fcache.page = page;
    memcpy(insn, g_fcache.host + (addr - page), sizeof *insn);
    return true;
}
void tlb_flush_all(void) {
    g_fcache.host = NULL;
    ++translation_generation;
}
