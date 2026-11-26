#include "mmu.h"
#include "runtime.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>

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
    u64 cursor, brk, data_bytes;
    unsigned refs;
};
#define ram (guest_process->memory->arena)
#define pages (guest_process->memory->table)
#define next_map (guest_process->memory->cursor)
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
        int old = pages[pos / PAGE].flags;
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
    if (account_pages(start, start + length, prot))
        return -ENOMEM;
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
    if (require_mapped) {
        for (size_t i = start / PAGE; i <= (end - 1) / PAGE; i++) {
            if (!(pages[i].flags & PRESENT))
                return -ENOMEM;
            if (prot >= 0 && (prot & PROT_WRITE) && (pages[i].flags & SHARED_READONLY))
                return -EACCES;
        }
    }
    if (account_pages(start, end, prot))
        return -ENOMEM;
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
            for (p = 0; p < size / PAGE && !(pages[addr / PAGE + p].flags & PRESENT); p++) {
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
    if (shared_file)
        for (size_t i = addr / PAGE; i < (addr + size) / PAGE; ++i)
            pages[i].flags |= SHARED_READONLY;
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
