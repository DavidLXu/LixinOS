#include "record-lock.h"
#include "ipc.h"
#include "native.h"
#include "identity.h"
#include "smp.h"
#include "kthread.h"
#include "vm.h"
#include "version.h"
#include "build-memory.h"

#define USER 0x44000000UL
#define BLOCK 0x200000UL
#define CODE (USER + 0x180000)
#define DATA (USER + 0x300000)
#define MAIL (USER + 0x3d8000)
#define UTILITY (MAIL - 4096)
#define BANKS NATIVE_TASKS
#define USER_STACK (USER + 2 * BLOCK)
#define PXN (1UL << 53)
#define UXN (1UL << 54)
#define NORMAL (1UL | (1UL << 10) | (3UL << 8))
#define EL0 (1UL << 6)
#define RO (1UL << 7)

static int equal(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}
static u32 length(const char *s, u32 cap) {
    u32 n = 0;
    while (n < cap && s[n])
        n++;
    return n;
}
static u32 le32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u64 le64(const u8 *p) { return le32(p) | (u64)le32(p + 4) << 32; }
static int magic(struct file f, const char *s, u32 n) {
    if (f.size == 0xffffffff || f.size < n)
        return 0;
    for (u32 i = 0; i < n; i++)
        if (f.data[i] != (u8)s[i])
            return 0;
    return 1;
}

struct cpu_tables {
    u64 root[512], ram[512], extended[512], ram_high[512], wide_tables[4][512];
};
static struct cpu_tables cpu_tables[2] __attribute__((aligned(4096)));
extern void enable_mmu(u64 *table, void (*vectors)(void), u64 high), native_vectors(void),
    sync_image(void);
extern long enter_el0(u64 a, u64 b, u64 c, u64 d);
#define bank (task_active())
static u8 *physical(void) {
    return bank < 8 ? (u8 *)(0x45000000UL + bank * (2 * BLOCK))
                    : (u8 *)(0x48000000UL + (bank - 8) * (2 * BLOCK));
}
#define ARENA 0x68000000UL
#define BUILD_FRAME_COUNT ((BUILD_RAM + GIB - BUILD_FRAMES) / BLOCK)
#define HEAP_FRAMES (build_volume ? BUILD_FRAME_COUNT : 144)
static u8 heap_used[BUILD_FRAME_COUNT];
static unsigned frame_hint;
static u64 frame_address(unsigned frame) {
    if (build_volume)
        return BUILD_FRAMES + frame * BLOCK;
    if (frame >= 80) return 0x98000000UL + (frame - 80) * BLOCK;
    return frame < 16 ? 0x66000000UL + frame * BLOCK
                      : 0x70000000UL + (frame - 16) * BLOCK;
}
enum { CANVAS_FIXED = 1, CANVAS_RESIZABLE = 2 };
enum { GRAPHICS_ARENA, LIBC_ARENA, EXTENDED_ARENA, PROGRAM_ARENA, WIDE_ARENA, ARENAS };
static const u64 arena_base[ARENAS] = {ARENA, 0x78000000UL, 0xc0000000UL, 0xa0000000UL, 64 * GIB};
static const unsigned build_blocks[ARENAS] = {64, 320, 512, 256, 2048};

static struct frame_map {
    u16 frames[2048];
    unsigned count, sealed;
} heaps[BANKS][ARENAS];
static unsigned mapped[2][ARENAS];
static unsigned mapped_owner[2]={BANKS,BANKS};
static struct vm_space vm_spaces[BANKS];
static void vm_bind_cpu(unsigned cpu, const struct vm_space *space) {
    for (unsigned i = 0; i < LX_VM_DIRS; ++i)
        cpu_tables[cpu].root[(LX_VM_BASE / GIB) + i] =
            space && space->dirs[i] ? (u64)space->dirs[i] | 3 : 0;
}
void vm_publish(unsigned owner, const struct vm_space *space) {
    for (unsigned cpu = 0; cpu < 2; ++cpu)
        if (mapped_owner[cpu] == owner) vm_bind_cpu(cpu, space);
}
int vm_memory_low(void) {
    unsigned free = 0;
    for (unsigned i=0; i<HEAP_FRAMES && free<16; ++i) free += !heap_used[i];
    return free < 16;
}
void *vm_allocate(u64 bytes) {
    if (bytes != BLOCK) return file_buffer(bytes);
    for (unsigned steps = 0; steps < HEAP_FRAMES; ++steps) {
        if (frame_hint >= HEAP_FRAMES) frame_hint = 0;
        unsigned frame = frame_hint++;
        if (!heap_used[frame]) {
            heap_used[frame] = 1;
            return (void *)frame_address(frame);
        }
    }
    return 0;
}
void vm_release(void *ptr, u64 bytes) {
    if (bytes != BLOCK) { release_buffer(ptr, bytes); return; }
    u64 pa = (u64)ptr;
    unsigned frame = build_volume ? (pa - BUILD_FRAMES) / BLOCK :
        pa >= 0x98000000UL ? 80 + (pa - 0x98000000UL) / BLOCK :
        pa >= 0x70000000UL ? 16 + (pa - 0x70000000UL) / BLOCK : (pa - 0x66000000UL) / BLOCK;
    heap_used[frame] = 0;
}
static void map_frame(unsigned cpu,unsigned kind, unsigned i, u64 value) {
    struct cpu_tables *t = &cpu_tables[cpu];
    u64 addr = arena_base[kind] + i * BLOCK;
    u64 *entries = addr >= 64 * GIB  ? t->wide_tables[(addr - 64 * GIB) / GIB]
                   : addr >= 3 * GIB ? t->extended
                   : addr >= 2 * GIB ? t->ram_high
                                     : t->ram;
    entries[(addr % GIB) / BLOCK] = value;
}
static void map_heap_cpu(unsigned cpu,unsigned id) {
    for (unsigned kind = 0; kind < ARENAS; kind++) {
        struct frame_map *h = &heaps[id][kind];
        for (unsigned i = 0; i < h->count; i++)
            map_frame(cpu,kind, i,
                      frame_address(h->frames[i]) | NORMAL | EL0 | PXN |
                          (i < h->sealed ? RO : UXN));
        for (unsigned i = h->count; i < mapped[cpu][kind]; i++)
            map_frame(cpu,kind, i, 0);
        mapped[cpu][kind] = h->count;
    }
}
static void map_heap(unsigned id) {
    for (unsigned cpu=0; cpu<2; cpu++)
        if (mapped_owner[cpu]==id) map_heap_cpu(cpu,id);
}
static long grow_heap(unsigned kind, u64 size) {
    unsigned limit = build_volume ? build_blocks[kind] : kind <= PROGRAM_ARENA ? 64 : 0;
    if (size > limit * BLOCK)
        return -1;
    struct frame_map *heap = &heaps[bank][kind];
    unsigned count = (size + BLOCK - 1) / BLOCK;
    if (count <= heap->count)
        return arena_base[kind];
    unsigned before = heap->count;
    for (unsigned steps = 0; heap->count < count && steps < HEAP_FRAMES; steps++) {
        if (frame_hint >= HEAP_FRAMES)
            frame_hint = 0;
        unsigned i = frame_hint++;
        if (!heap_used[i]) {
            heap_used[i] = 1;
            heap->frames[heap->count++] = i;
            memset((void *)frame_address(i), 0, BLOCK);
        }
    }
    if (heap->count < count) {
        while (heap->count > before)
            heap_used[heap->frames[--heap->count]] = 0;
        return -1;
    }
    map_heap(bank);
    sync_image();
    return arena_base[kind];
}
void smp_bind_kernel(void) {
    unsigned id = task_active();
    if (id < NATIVE_TASKS) {
        disk_select_task(id);
        display_select(process_pane());
    }
}
void select_bank(unsigned id) {
    if (id >= NATIVE_TASKS) return;
    smp_bind_kernel();
    mapped_owner[smp_cpu()]=id;
    map_heap_cpu(smp_cpu(),id);
    shared_switch(id);
    vm_bind_cpu(smp_cpu(), &vm_spaces[id]);
    u64 pa = (u64)physical();
    cpu_tables[smp_cpu()].ram[32] = pa | NORMAL | EL0 | RO | PXN;
    cpu_tables[smp_cpu()].ram[33] = (pa + BLOCK) | NORMAL | EL0 | PXN | UXN;
    sync_image();
}

struct redirected_file {
    unsigned references, size, pos, capacity;
    int failed;
    u8 *data;
    char path[513];
};
static struct redirected_file redirected[2 * BANKS];
#define FD_LIMIT LX_DESCRIPTOR_LIMIT
#define HANDLE_COUNT (FD_LIMIT - 3)
struct process {
    u32 gpu_context, audio_stream;
    char cwd[513], path[513], args[32768];
    char executable[513], start_args[32768], git_meta[513];
    unsigned start_used, start_argc, pane, start_flags;
    unsigned null_streams, env_result;
    char environment[8192];
    int background, agent, agent_child;
    char scope[513];
    unsigned command_output;
    u64 command_deadline;
    struct {
        const u8 *input;
        u8 *output;
        unsigned remaining, used, overflow;
        u64 deadline;
    } test;
    struct pipe *in, *out, *err, *pending_pipe;
    char next_output[513], next_input[513];
    unsigned append, closed;
    struct redirected_file *input, *capture;
    char edit_path[513];
    struct file original;
    int foreground, nano;
    unsigned argc, used, canvas, windows, clipboard_options;
    struct {
        int slot;
        long pid;
    } sessions[BANKS];
    long status;
    const char *error;
    unsigned long record_owner;
    struct {
        struct file file;
        unsigned pos;
        int socket, append;
        struct pipe *pipe;
        struct local_socket *local;
        struct shared_file *shared;
        int writer;
        char path[513];
    } handles[HANDLE_COUNT];
};
static struct process processes[BANKS];
int smp_clock_allowed(void) {
    const struct process *p = &processes[bank];
    return !p->test.deadline && !p->command_deadline && !p->scope[0] && !p->agent_child && !task_stopped();
}
static int under(const char *path, const char *root) {
    unsigned n=length(root,513);
    return length(path,513)>=n && !memcmp(path,root,n) && (!path[n] || path[n]=='/');
}
int agent_path_allowed(const char *path, int writing) {
    const char *root=processes[bank].scope;
    if (!root[0]) return 1;
    if (under(path,root)) return 1;
    return !writing && (under(path,"/bin") || under(path,"/lib") || under(path,"/src") ||
        under(path,"/libexec") || under(path,"/proc") || equal(path,"/"));
}
u64 process_private_bytes(unsigned slot) {
    u64 blocks=2;
    for(unsigned k=0;k<ARENAS;k++) blocks+=heaps[slot][k].count;
    return blocks*BLOCK;
}
const char *process_task_name(unsigned slot) { return processes[slot].executable; }
static void inherit_scope(struct process *child, struct process *parent) {
    child->env_result = parent->env_result;
    if (child->env_result) memcpy(child->environment,parent->environment,child->env_result-1);
    if (parent->scope[0]) {
        memcpy(child->scope,parent->scope,length(parent->scope,513)+1);
        child->command_deadline=parent->command_deadline;
    }
}
unsigned process_pane(void) { return processes[bank].pane; }

static int resolve(const char *cwd, const char *path, char out[513]) {
    if (!*path)
        return 0;
    unsigned n = 0;
    if (*path != '/') {
        n = length(cwd, 513);
        if (n > 512)
            return 0;
        memcpy(out, cwd, n);
    }
    if (!n)
        out[n++] = '/';
    while (*path) {
        if (*path == '/') {
            path++;
            continue;
        }
        unsigned m = 0;
        while (path[m] && path[m] != '/')
            m++;
        if (m == 2 && path[0] == '.' && path[1] == '.') {
            while (n > 1 && out[n - 1] != '/')
                n--;
            if (n > 1)
                n--;
        } else if (!(m == 1 && path[0] == '.')) {
            if (n + (n > 1) + m > 512)
                return 0;
            if (n > 1)
                out[n++] = '/';
            memcpy(out + n, path, m);
            n += m;
        }
        path += m;
        if (*path) {
            struct file dir;
            out[n] = 0;
            // A scoped command must traverse the ancestors of its workspace.
            // Final lookups and directory listings still enforce the scope.
            const char *scope = processes[bank].scope;
            if (!(scope[0] && under(scope, out) && !equal(scope, out)) &&
                (!vfs_lookup(out, &dir) || dir.size != 0xffffffff))
                return 0;
        }
    }
    out[n] = 0;
    return 1;
}

static long reply(char *mail, const char *s) {
    unsigned n = length(s, 4096);
    memcpy(mail, s, n + 1);
    return n;
}

static long spawn(struct process *parent, unsigned flags);
static long process_request(struct process *p, unsigned op, unsigned index) {
    char *mail = (char *)MAIL, path[513];
    if (length(mail, 4097) == 4097)
        return -1;
    struct file f;
    switch (op) {
    case 0:
        if (!resolve(p->cwd, mail, path) || !vfs_lookup(path, &f))
            return -1;
        if (f.size == 0xffffffff)
            return reply(mail, "d");
        if (magic(f, "LXcmd1\n", 7) && f.size - 7 < 63) {
            mail[0] = '@';
            memcpy(mail + 1, f.data + 7, f.size - 7);
            mail[f.size - 6] = 0;
            return f.size - 6;
        }
        return reply(
            mail,
            magic(f, "LXE1", 4) || magic(f, "\177ELF", 4) || magic(f, "LXalias1\n", 9) ? "x" : "f");
    case 1: {
        const char *text = history_get(index);
        return text ? reply(mail, text) : -1;
    }
    case 2: {
        if (!resolve(p->cwd, mail, path))
            return -1;
        const char *prefix = mail + length(mail, 4096) + 1;
        unsigned n = length(prefix, 513);
        if (n > 512)
            return -1;
        const char *after = prefix + n + 1;
        unsigned a = length(after, 513);
        if (a > 512)
            return -1;
        char name[513], full[513];
        for (unsigned i = 0;; i++) {
            long size = vfs_entry(path, i, name);
            if (size == -1)
                return -1;
            unsigned len = length(name, 513), common = len < a ? len : a;
            int order = memcmp(name, after, common);
            if (len < n || memcmp(prefix, name, n) || order < 0 || (!order && len <= a))
                continue;
            int kind = size == -2 ? 1 : 0;
            if (!kind && resolve(path, name, full) && vfs_lookup(full, &f) &&
                (magic(f, "LXE1", 4) || magic(f, "\177ELF", 4) || magic(f, "LXcmd1\n", 7) ||
                 magic(f, "LXalias1\n", 9)))
                kind = 2;
            mail[0] = '0' + kind;
            memcpy(mail + 1, name, len + 1);
            return len + 1;
        }
    }
    case 3:
        return history_append(mail, length(mail, 513)) ? 0 : -1;
    case 4:
        if (!resolve(p->cwd, mail, p->path))
            return -1;
        p->used = p->argc = 0;
        return 0;
    case 5: {
        u32 n = length(mail, 4097) + 1;
        if (n > sizeof(p->args) - p->used || p->argc >= 512)
            return -1;
        memcpy(p->args + p->used, mail, n);
        p->used += n;
        p->argc++;
        return 0;
    }
    case 7:
        return p->status;
    case 16:
        return ssh_mode() ? mux_next() : -1;
    case 17:
        if (!ssh_mode())
            return -1;
        mux_close();
        return 0;
    case 18:
        return spawn(p, index);
    case 19:
        return task_wait(index);
    case 20:
        pipe_ref(p->pending_pipe, 0, -1);
        p->pending_pipe = 0;
        p->next_output[0] = p->next_input[0] = 0;
        return 0;
    case 21:
        if (!resolve(p->cwd, mail, p->next_output) ||
            (vfs_lookup(p->next_output, &f) && f.size == 0xffffffff))
            return -1;
        if (proc_path(p->next_output)) {
            p->next_output[0] = 0;
            p->error = "read-only filesystem";
            return -1;
        }
        p->append = !!index;
        return 0;
    case 22:
        if (!resolve(p->cwd, mail, p->next_input) || !vfs_lookup(p->next_input, &f) ||
            f.size == 0xffffffff)
            return -1;
        return 0;
    case 23:
        return task_pid();
    case 9:
        return 0; /* foreground console has no captured stream */
    case 13:
        return reply(mail, p->error ? p->error : "service unavailable");
    case 14:
        if (!resolve(p->cwd, mail, path) || !vfs_lookup(path, &f) || f.size != 0xffffffff)
            return -1;
        memcpy(p->cwd, path, length(path, 513) + 1);
        return reply(mail, path);
    default:
        return -1;
    }
}

/* Snapshot-backed descriptors keep their original contents across a commit. */
#define CHUNK 65536u
#define CHUNKS (build_volume ? GIB / CHUNK : NATIVE_FILE_POOL / CHUNK)
#define FILE_POOL_BASE (build_volume ? BUILD_BUFFERS : 0x5a000000UL)
static u8 chunks_used[GIB / CHUNK];
u8 *file_buffer(u32 size) {
    unsigned need = (size + CHUNK - 1) / CHUNK, run = 0;
    if (!need)
        need = 1;
    for (unsigned i = 0; i < CHUNKS; i++) {
        run = chunks_used[i] ? 0 : run + 1;
        if (run == need) {
            unsigned first = i + 1 - need;
            memset(chunks_used + first, 1, need);
            return (u8 *)(FILE_POOL_BASE + first * CHUNK);
        }
    }
    return 0;
}
void release_buffer(const u8 *ptr, unsigned bytes) {
    if (!ptr)
        return;
    unsigned first = ((u64)ptr - FILE_POOL_BASE) / CHUNK;
    unsigned count = (bytes + CHUNK - 1) / CHUNK;
    memset(chunks_used + first, 0, count ? count : 1);
}
void *gpu_allocate(u32 n) {
    u8 *p = file_buffer(n);
    if (p) memset(p, 0, n);
    return p;
}
void gpu_free(void *p, u32 n) { release_buffer(p, n); }
static u8 *grow_file_buffer(u8 *data, unsigned old, unsigned used, unsigned capacity) {
    unsigned first = ((u64)data - FILE_POOL_BASE) / CHUNK;
    unsigned allocated = old ? (old + CHUNK - 1) / CHUNK : 1;
    unsigned needed = (capacity + CHUNK - 1) / CHUNK;
    unsigned i = first + allocated;
    if (first + needed <= CHUNKS) {
        while (i < first + needed && !chunks_used[i])
            i++;
        if (i == first + needed) {
            memset(chunks_used + first + allocated, 1, needed - allocated);
            return data;
        }
    }
    u8 *next = file_buffer(capacity);
    if (!next)
        return 0;
    memcpy(next, data, used);
    release_buffer(data, old);
    return next;
}
u64 heap_bytes(void) {
    u64 used = 0;
    for (unsigned i = 0; i < HEAP_FRAMES; i++)
        if (heap_used[i])
            used += BLOCK;
    for (unsigned i = 0; i < CHUNKS; i++)
        if (chunks_used[i])
            used += CHUNK;
    return used;
}
u64 native_ram_bytes(void) { return build_volume ? BUILD_RAM : NATIVE_RAM_BYTES; }
u64 heap_capacity_bytes(void) { return HEAP_FRAMES * BLOCK + (u64)CHUNKS * CHUNK; }
static void metrics(u64 *s) {
    s[0] = clock_ticks() / clock_hz();
    s[1] = heap_bytes();
    s[2] = heap_capacity_bytes() - s[1];
    file_stats(s + 3);
    s[6] = 128;
    s[7] = native_ram_bytes();
}
static long system_info(char *out, unsigned op) {
    u64 used = disk_usage(), cap = disk_capacity();
    if (op == 6)
        return task_list(out);
    if (op == 8) {
        char *p = add_number(add_text(out, "architecture=aarch64\nvcpus="), smp_cpus());
        p = add_text(p, "\nuptime_seconds=");
        p = add_number(p, clock_ticks() / clock_hz());
        p = add_text(p, "\narchitectural_timer_hz=");
        p = add_number(p, clock_hz());
        p = add_text(p, "\nguest_busy_percent=");
        u64 start = clock_ticks(), idle = idle_ticks(), hz = clock_hz(), before = smp_busy_ticks();
        while (clock_ticks() - start < hz / 4)
            idle_slice();
        u64 duration = clock_ticks() - start, wait = idle_ticks() - idle;
        u64 remote = smp_busy_ticks() - before;
        if (remote > duration) remote = duration;
        p = add_number(p, ((duration > wait ? duration - wait : 0) + remote) * 100 / (duration * smp_cpus()));
        p = add_text(p, "\nsample_ms=");
        p = add_number(p, duration * 1000 / hz);
        return add_text(p, "\nscope=guest_scheduler") - out;
    }
    if (op == 7) {
        char *p = add_text(out, "Filesystem Capacity Used Available\n/ ");
        p = add_number(p, cap);
        *p++ = ' ';
        p = add_number(p, used);
        *p++ = ' ';
        p = add_number(p, cap - used);
        return add_text(p, "\n") - out;
    }
    u64 s[8];
    metrics(s);
    char *p = add_text(out, "name=LixinOS\nrelease=" KERNEL_RELEASE "\narchitecture=aarch64\nlinux_kernel="
                            "false\nplatform=QEMU virt guest");
    const char *labels[] = {"guest_ram_bytes",
                            "heap_used_bytes",
                            "heap_free_bytes",
                            "scheduler_timer_interrupts",
                            "committed_snapshot_generation",
                            "files",
                            "directories",
                            "file_content_bytes"};
    const u64 vals[] = {s[7], s[1], s[2], timer_count(), disk_generation(), s[3], s[4], s[5]};
    for (unsigned i = 0; i < 8; i++) {
        *p++ = '\n';
        p = add_text(p, labels[i]);
        *p++ = '=';
        p = add_number(p, vals[i]);
    }
    return p - out;
}
static int valid_text(const u8 *s, u32 n) {
    for (u32 i = 0; i < n;) {
        unsigned c = s[i++];
        if (!c)
            return 0;
        if (c < 128)
            continue;
        unsigned extra = c >= 0xc2 && c <= 0xdf   ? 1
                         : c >= 0xe0 && c <= 0xef ? 2
                         : c >= 0xf0 && c <= 0xf4 ? 3
                                                  : 0;
        if (!extra || extra > n - i)
            return 0;
        unsigned first = s[i];
        if ((c == 0xe0 && first < 0xa0) || (c == 0xed && first >= 0xa0) ||
            (c == 0xf0 && first < 0x90) || (c == 0xf4 && first >= 0x90))
            return 0;
        while (extra--)
            if ((s[i++] & 0xc0) != 0x80)
                return 0;
    }
    return 1;
}
static long editor_save(struct process *p, u64 size) {
    if (proc_path(p->edit_path))
        return 4;
    if (!p->edit_path[0] || size > EDITOR_FILE_LIMIT)
        return 1;
    struct file f;
    if (vfs_lookup(p->edit_path, &f)) {
        if (!p->original.data || p->original.size != f.size ||
            memcmp(p->original.data, f.data, f.size))
            return 2;
    } else if (p->original.data)
        return 2;
    u8 *next = file_buffer(size);
    if (!next)
        return 4;
    memcpy(next, (const void *)ARENA, size);
    if (!store_file(p->edit_path, next, size)) {
        release_buffer(next, size);
        return 4;
    }
    release_buffer(p->original.data, p->original.size);
    p->original = (struct file){next, size};
    return 0;
}
static int handle_used(struct process *p, unsigned i) {
    return p->handles[i].file.data || p->handles[i].pipe || p->handles[i].local ||
           p->handles[i].path[0] || p->handles[i].socket || p->handles[i].shared;
}

static void close_handle(struct process *p, unsigned i) {
    struct file *f = &p->handles[i].file;
    if (p->handles[i].socket) {
        socket_release(p->handles[i].socket - 1);
        p->handles[i].socket = 0;
    }
    if (p->handles[i].pipe) {
        pipe_ref(p->handles[i].pipe, p->handles[i].writer, -1);
        p->handles[i].pipe = 0;
    }
    if (p->handles[i].local) {
        local_close(p->handles[i].local);
        p->handles[i].local = 0;
    }
    if (p->handles[i].shared) {
        shared_release(p->handles[i].shared);
        p->handles[i].shared = 0;
    }
    p->handles[i].path[0] = 0;
    if (!f->data)
        return;
    release_buffer(f->data, f->size);
    f->data = 0;
}
int uses_terminal_pointer(void) {
    if (processes[bank].background || processes[bank].canvas) return 0;
    if (popup_terminal()) return 1;
    /* A preempted window client uses pipes. Its timer tick must leave
       tablet events for the desktop that owns this pane's canvas. */
    for (unsigned i = 0; i < BANKS; i++)
        if (processes[i].pane == process_pane() && processes[i].canvas) return 0;
    return 1;
}
int graphical_shortcuts(void) {
    if (popup_terminal()) return display_editor_active();
    for (unsigned i = 0; i < BANKS; ++i)
        if (processes[i].pane == process_pane() && processes[i].canvas) return 1;
    return 0;
}
int display_busy(void) {
    for (unsigned i = 0; i < BANKS; i++)
        if (processes[i].canvas == CANVAS_FIXED)
            return 1;
    return 0;
}
int editing_file(void) { return !!processes[bank].edit_path[0]; }
int canvas_clipboard_options(void) { return processes[bank].canvas ? processes[bank].clipboard_options : 0; }
static int readable(u64 address, u64 size) {
    if (vm_readable(&vm_spaces[bank], address, size)) return 1;
    if (address >= USER + BLOCK && address <= UTILITY && size <= UTILITY - address)
        return 1;
    for (unsigned kind = 0; kind < ARENAS; kind++) {
        u64 base = arena_base[kind], end = base + heaps[bank][kind].count * BLOCK;
        if (address >= base && address <= end && size <= end - address)
            return 1;
    }
    return 0;
}
static long save_output(const char *path, u64 descriptor) {
    if (proc_path(path))
        return -1;
    if (descriptor == ~0UL) {
        const u64 *range = (const u64 *)(UTILITY + 520);
        if (range[1] > NATIVE_FILE_LIMIT || !readable(range[0], range[1]))
            return -1;
        return store_file(path, (const u8 *)range[0], range[1]) ? 0 : -1;
    }
    unsigned size = descriptor;
    if (!(descriptor >> 32))
        return size <= 32768 && store_file(path, (const u8 *)MAIL, size) ? 0 : -1;
    const u8 *table = (const u8 *)(descriptor >> 32);
    if (size > NATIVE_FILE_LIMIT || !readable((u64)table, 4))
        return -1;
    unsigned count = le32(table);
    if (count > size || !readable((u64)table, 4UL + count * 12UL))
        return -1;
    u8 *result = file_buffer(size);
    if (!result)
        return -1;
    memset(result, 0, size);
    u32 remaining = size;
    long status = -1;
    for (unsigned i = 0; i < count; i++) {
        const u8 *part = table + 4 + i * 12;
        u32 off = le32(part), addr = le32(part + 4), len = le32(part + 8);
        if (!len || len > remaining || off > size || len > size - off || !readable(addr, len))
            goto done;
        memcpy(result + off, (const void *)(u64)addr, len);
        remaining -= len;
    }
    status = store_file(path, result, size) ? 0 : -1;
done:
    release_buffer(result, size);
    return status;
}

static struct redirected_file *redirect_open(const char *path, int writing, int append) {
    struct file f = {0};
    if (writing && proc_path(path))
        return 0;
    if (vfs_lookup(path, &f)) {
        if (f.size > NATIVE_FILE_LIMIT)
            return 0;
    } else if (!writing)
        return 0;
    unsigned capacity = !writing || append ? f.size : 0;
    for (unsigned i = 0; i < 2 * BANKS; i++) {
        struct redirected_file *d = &redirected[i];
        if (d->references)
            continue;
        u8 *data = file_buffer(capacity);
        if (!data)
            return 0;
        *d = (struct redirected_file){.references = 1, .capacity = capacity, .data = data};
        if (!writing || append) {
            memcpy(data, f.data, f.size);
            d->size = f.size;
        }
        if (writing)
            add_text(d->path, path);
        return d;
    }
    return 0;
}
static int redirect_close(struct redirected_file *d, int publish) {
    if (!d || --d->references)
        return 1;
    int ok = !publish || !d->path[0] || (!d->failed && store_file(d->path, d->data, d->size));
    release_buffer(d->data, d->capacity);
    return ok;
}
static long stream_read(struct process *p, u8 *dst, unsigned n) {
    if (!n)
        return 0;
    if (p->closed & 1)
        return -1;
    if (p->null_streams & 1) return 0;
    if (p->in)
        return pipe_read(p->in, dst, n);
    if (p->input) {
        unsigned left = p->input->size - p->input->pos;
        if (n > left)
            n = left;
        memcpy(dst, p->input->data + p->input->pos, n);
        p->input->pos += n;
        return n;
    }
    if (p->background)
        return 0;
    int key = console_read(0xffffffff);
    if (key == 256)
        return 0;
    dst[0] = key;
    return 1;
}
static long stream_write(struct process *p, unsigned fd, const u8 *src, unsigned n, int raw) {
    if (p->scope[0]) {
        if (p->command_output>65536 || n>65536-p->command_output) {
            p->command_deadline=1;return -1;
        }
        p->command_output+=n;
    }
    if (p->closed & (1u << fd))
        return -1;
    if (p->null_streams & (1u << fd)) return n;
    if (fd == 2 && p->err) return pipe_write(p->err, src, n);
    if (fd == 1 && p->out)
        return pipe_write(p->out, src, n);
    if (fd == 1 && p->capture) {
        struct redirected_file *capture = p->capture;
        if (capture->failed)
            return -1;
        if (n > NATIVE_FILE_LIMIT - capture->size) {
            serial_text("file too large\n");
            capture->failed = 1;
            return -1;
        }
        unsigned end = capture->size + n;
        if (end > capture->capacity) {
            unsigned capacity = capture->capacity > CHUNK ? capture->capacity : CHUNK;
            while (capacity < end)
                capacity = capacity > NATIVE_FILE_LIMIT / 2 ? NATIVE_FILE_LIMIT : 2 * capacity;
            u8 *data = grow_file_buffer(capture->data, capture->capacity, capture->size, capacity);
            if (!data) {
                capture->failed = 1;
                return -1;
            }
            capture->data = data;
            capture->capacity = capacity;
        }
        memcpy(capture->data + capture->size, src, n);
        capture->size = end;
        return n;
    }
    if (popup_closed())
        return n;
    for (unsigned i = 0; i < n; i++) {
        if (!raw && src[i] == '\n')
            serial_put('\r');
        serial_put(src[i]);
    }
    return n;
}
static void close_stream(struct process *p, unsigned fd) {
    p->closed |= 1u << fd;
    if (!fd) {
        pipe_ref(p->in, 0, -1);
        p->in = 0;
    }
    if (fd == 2) { pipe_ref(p->err, 1, -1); p->err = 0; }
    if (fd == 1) {
        pipe_ref(p->out, 1, -1);
        p->out = 0;
    }
}

/* Forward the replaceable EL0 policy runner's mailbox to its parent broker.
 * The child owns its own bank, including the mailbox across pipe waits. */
static int read_agent_pipe(struct process *p, u8 *data, unsigned size) {
    while (size) {
        long got = stream_read(p, data, size);
        if (got <= 0) return 0;
        data += got;
        size -= got;
    }
    return 1;
}
static long exchange_agent_runner(struct process *p, u64 value) {
    if (!p->agent_child || !equal(p->executable, "/bin/agent-run") || !p->in || !p->out ||
        value >> 8 > 32768) return -1;
    u32 header[2] = {value & 255, value >> 8}, size;
    if (stream_write(p,1,(const u8 *)header,sizeof(header),1) != sizeof(header) ||
        (header[1] && stream_write(p,1,(const u8 *)MAIL,header[1],1) != header[1]) ||
        !read_agent_pipe(p,(u8 *)&size,sizeof(size)) || size > 32768 ||
        !read_agent_pipe(p,(u8 *)MAIL,size)) return -1;
    return size;
}

static long local_call(struct process *p, u64 value, u8 *mail) {
    unsigned command = value >> 56, fd = (value >> 32) & 255, argument = (u32)value;
    if (!command || command == 12) return 1;
    if (command <= 2 || command == 4 || command == 5) {
        unsigned left = 0;
        while (left < HANDLE_COUNT && handle_used(p, left)) left++;
        if (left == HANDLE_COUNT) return -24;
        struct local_socket *socket;
        int result;
        if (command == 1) {
            unsigned right = left + 1;
            while (right < HANDLE_COUNT && handle_used(p, right)) right++;
            if (right == HANDLE_COUNT) return -24;
            struct local_socket *other;
            result = local_pair(task_pid(), task_pid(), &socket, &other);
            if (result) return result;
            p->handles[left].local = socket;
            p->handles[right].local = other;
            return ((u64)(left + 3) << 32) | (right + 3);
        }
        if (command == 5) {
            if (fd < 3 || fd >= FD_LIMIT || !p->handles[fd-3].local) return -22;
            result = local_accept(p->handles[fd-3].local, &socket);
        } else {
            if (!argument || argument > 108) return -22;
            if (command == 2) result = local_bind(mail, argument, task_pid(), &socket);
            else result = local_connect(mail, argument, task_pid(), &socket);
        }
        if (result) return result;
        p->handles[left].local = socket;
        return left + 3;
    }
    if (fd < 3 || fd >= FD_LIMIT || !p->handles[fd-3].local)
        return command == 3 ? -22 : -107;
    if (command == 3) return local_listen(p->handles[fd-3].local, argument);
    if (command >= 6 && command <= 9) return local_io(p->handles[fd-3].local, command, mail, argument);
    if (command == 10 || command == 11) {
        u32 *control = (void *)(mail + LX_IPC_CONTROL);
        unsigned limit = control[0];
        if (argument > LX_IPC_CONTROL || limit > LX_IPC_RIGHTS) return -22;
        struct shared_file *files[LX_IPC_RIGHTS] = {0};
        if (command == 10) {
            for (unsigned i = 0; i < limit; i++) {
                unsigned source = control[i + 2];
                if (source < 3 || source >= FD_LIMIT || !handle_used(p, source - 3)) return -9;
                files[i] = p->handles[source - 3].shared;
                if (!files[i]) return -95;
            }
            return local_write_rights(p->handles[fd-3].local, mail, argument, files);
        }
        long size = local_read_rights(p->handles[fd-3].local, mail, argument, files);
        if (size < 0) return size;
        unsigned count = 0, truncated = 0, slot = 0;
        for (unsigned i = 0; i < LX_IPC_RIGHTS && files[i]; i++) {
            while (slot < HANDLE_COUNT && handle_used(p, slot)) slot++;
            if (count < limit && slot < HANDLE_COUNT) {
                p->handles[slot].shared = files[i];
                control[2 + count++] = slot++ + 3;
            } else {
                truncated = 1;
                shared_release(files[i]);
            }
        }
        control[0] = count;
        control[1] = truncated;
        return size;
    }
    return -38;
}

static long shared_call(struct process *p, u64 value, u8 *mail) {
    unsigned command = value >> 56, fd = (value >> 32) & 255, argument = (u32)value;
    const u64 *words = (const u64 *)mail;
    if (command == LX_SHARED_SPACE) {
        unsigned free_chunks = 0;
        for (unsigned i = 0; i < CHUNKS; i++) free_chunks += !chunks_used[i];
        ((u64 *)mail)[0] = (u64)CHUNKS * CHUNK;
        ((u64 *)mail)[1] = (u64)free_chunks * CHUNK;
        return 0;
    }
    if (command == 5) return shared_unmap(bank, words[0], words[1]);
    struct shared_file *file = fd >= 3 && fd < FD_LIMIT ? p->handles[fd - 3].shared : 0;
    if (command == 0 || command == 1) {
        if (command == 1 && !file) return -9;
        unsigned slot = 0;
        while (slot < HANDLE_COUNT && handle_used(p, slot)) slot++;
        if (slot == HANDLE_COUNT) return -24;
        if (file && command == 1) shared_ref(file);
        else file = shared_create();
        if (!file) return -23;
        p->handles[slot].shared = file;
        return slot + 3;
    }
    if (!file) return -9;
    switch (command) {
    case 2: return shared_resize(file, argument);
    case 3: return shared_size(file);
    case 4: return shared_map(bank, file, words[0], words[1], words[2]);
    case 6: case 7: return argument <= 4096 ? shared_transfer(file, mail, argument, command == 7) : -22;
    case 8: return shared_seek(file, (long)words[0], argument);
    default: return -22;
    }
}

static long syscall_service(u64 value, u64 op) {
    struct process *p = &processes[bank];
    char *mail = (char *)UTILITY, path[513];
    unsigned fd = value >> 32, n = (u32)value;
    struct file f;
    if (p->agent_child && equal(p->executable,"/bin/agent-run") && op != 12) return -1;
    if (p->scope[0] && ((op>=19 && op<=21) || (op>=25 && op<=28) || op==48 || op==58 || op==59 || op==60 || op==63 ||
        op==33 || op==34 || op==35 || (op==18 && ((u32)value==3 || (u32)value==16 ||
        (u32)value==17 || ((u32)value==18 && ((value>>32)&4)))))) return -1;
    if (p->test.deadline) {
        switch (op) {
        case 0: if (!p->test.remaining) return 256;
            p->test.remaining--; return *p->test.input++;
        case 1: case 11:
            if (p->test.used == 2048) {
                p->test.overflow=1;
                return -1;
            }
            p->test.output[p->test.used++] = value; return (u8)value;
        case 3: return 0;
        case 5: return 1;
        case 38: return value ? -1 : clock_ticks() / (clock_hz()/1000);
        default: return -1;
        }
    }
    switch (op) {
    case 65: {
        if (value == LX_LOCK_PROBE) return LX_LOCK_VERSION;
        if (fd < 3 || fd >= FD_LIMIT || !p->handles[fd-3].path[0]) return -9;
        const char *name = p->handles[fd-3].path;
        return record_call(&p->record_owner, name, length(name, 513), !p->handles[fd-3].file.data,
                           n, (struct lx_record_request *)mail);
    }
    case 64:
        if (value==18) return disk_checkpoint()?0:-5;
        if (value>=12 && value<=17) {
            char canonical[513];
            if (length((char *)mail+16,513)>512 || !resolve(p->cwd,(char *)mail+16,canonical)) return -22;
            return disk_mode(canonical,value,(unsigned *)mail);
        }
        return id_call(task_active(),value,(unsigned *)mail);
    case 63: return audio_call(&p->audio_stream, value, (const u8 *)mail);
    case 62: {
        u64 args[6];
        memcpy(args, mail, sizeof(args));
        struct file source = {0};
        if (value == LX_VM_FILE) {
            if (args[4] < 3 || args[4] >= FD_LIMIT) return -9;
            unsigned index = args[4] - 3;
            if (p->handles[index].path[0] && !p->handles[index].file.data) {
                if (!vfs_lookup(p->handles[index].path, &source) || source.size == 0xffffffff) return -9;
            } else {
                source = p->handles[index].file;
                if (!source.data) return -9;
            }
        }
        return vm_call(&vm_spaces[bank], bank, value, args, source.data, source.size);
    }
    case 58: return local_call(p, value, (u8 *)mail);
    case 60: return gpu_call(&p->gpu_context, value, (u8 *)0x443d8000UL);
    case 59: return shared_call(p, value, (u8 *)mail);
    case 0: {
        u8 c;
        if (stream_read(p, &c, 1) != 1)
            return 256;
        return value == 0 && c == 4 && !p->input && !p->in ? 256 : c;
    }
    case 1:
    case 11: {
        u8 c = value;
        return stream_write(p, op == 1 ? 1 : 2, &c, 1, !!(value & 256)) == 1 ? c : -1;
    }
    case 3:
    case 5:
        return 0;
    case 12:
        return exchange_agent_runner(p,value);
    case 15:
        return editor_save(p, value);
    case 16:
        if (!p->foreground)
            return -1;
        if (value == 1) {
            u64 *words = (u64 *)MAIL;
            words[0] = clock_ticks();
            words[1] = clock_hz();
            words[2] = heap_capacity_bytes() - heap_bytes();
            words[3] = smp_cpus();
            words[4] = smp_idle_ticks(0);
            words[5] = words[3] > 1 ? smp_idle_ticks(1) : 0;
            return 48 + task_resources(words + 6);
        }
        metrics((u64 *)MAIL);
        return 0;
    case 17:
        return p->edit_path[0] && grow_heap(GRAPHICS_ARENA, value) >= 0 ? 0 : -1;
    case 44: {
        unsigned len = length(mail, 513);
        if (len > 512)
            return -1;
        memcpy(p->path, "/bin/bash", 10);
        memcpy(p->args, "bash\0-c", 8);
        memcpy(p->args + 8, mail, len + 1);
        p->argc = 3;
        p->used = 9 + len;
        long pid = spawn(p, 8);
        return pid < 0 ? -1 : task_wait(pid);
    }
    case 6:
        if (value == 10 && *p->git_meta)
            return grow_heap(LIBC_ARENA, *(unsigned *)0x443d8000UL);
        return git_call(bank, p->git_meta, value);
    case 45:
        return file_operation(bank, p->cwd, value, mail);
    case 42:
        if (value == 6)
            return FD_LIMIT;
        if (value == 5) return reply(mail, p->executable);
        if (!value)
            return reply(mail, p->cwd);
        if (length(mail, 513) > 512 || !resolve(p->cwd, mail, path))
            return -22;
        if (value == 1) {
            if (!vfs_lookup(path, &f) || f.size != 0xffffffff)
                return -20;
            memcpy(p->cwd, path, length(path, 513) + 1);
            return 0;
        }
        if (value >= 2 && value <= 4) {
            if (value == 2) {
                /* create_dir_all retries the parent specifically on ENOENT. */
                char parent[513];
                memcpy(parent, path, strlen(path) + 1);
                unsigned end = strlen(parent);
                while (end > 1 && parent[end - 1] != '/')
                    end--;
                parent[end > 1 ? end - 1 : 1] = 0;
                if (!vfs_lookup(parent, &f))
                    return -2;
                if (f.size != 0xffffffff)
                    return -20;
                if (vfs_lookup(path, &f))
                    return -17;
            }
            if (!batch_begin(bank))
                return -5;
            memcpy(mail, path, length(path, 513) + 1);
            unsigned edit = value == 2 ? 1 : value == 3 ? 3 : 259;
            if (file_operation(bank, p->cwd, edit, mail) < 0)
                return -5;
            return batch_commit(bank) ? 0 : -5;
        }
        return -22;
    case 25: {
        if (length(mail, 254) >= 254 || value > 65535 || !value)
            return -1;
        unsigned i = 0;
        while (i < HANDLE_COUNT && handle_used(p, i))
            i++;
        if (i == HANDLE_COUNT)
            return -1;
        int id = socket_connect(mail, value);
        if (id < 0)
            return -1;
        p->handles[i].socket = id + 1;
        return i + 3;
    }
    case 26:
    case 27: {
        int peek = op == 26 && (n & LX_TCP_PEEK_FLAG);
        if (peek) n &= ~LX_TCP_PEEK_FLAG;
        if (fd < 3 || fd >= FD_LIMIT || n > 4096 || !p->handles[fd - 3].socket)
            return -1;
        enum socket_operation action = op == 27 ? SOCKET_WRITE : peek ? SOCKET_PEEK : SOCKET_READ;
        return socket_io(p->handles[fd - 3].socket - 1, action, (u8 *)mail, n);
    }
    case 28:
        if (fd < 3 || fd >= FD_LIMIT || !p->handles[fd - 3].socket)
            return -1;
        close_handle(p, fd - 3);
        return 0;
    case 30:
        return value <= 4096 ? entropy_read((u8 *)mail, value) : -1;
    case 29:
        if (value == 11) return input_modifiers();
        if (value == 10) return smp_cpu();
        if (value == 9) return task_resources((u64 *)mail);
        if (value == 0)
            return net_info(mail);
        if (value == 1)
            return reply(mail, "LixinOS");
        if (value == 2)
            return reply(mail, KERNEL_RELEASE);
        if (value == 3)
            return reply(mail, "aarch64");
        if (value >= 5 && value <= 8)
            return system_info(mail, value);
        if (value == 4)
            return reply(mail, KERNEL_SUMMARY);
        return -1;
    case 38: return smp_clock_read(value);
    case 40: {
        if (length(mail, 513) > 512 || !resolve(p->cwd, mail, path) || proc_path(path))
            return -1;
        unsigned i = 0;
        while (i < HANDLE_COUNT && handle_used(p, i))
            i++;
        if (i == HANDLE_COUNT || (vfs_lookup(path, &f) && f.size == 0xffffffff))
            return -1;
        if ((!value || !vfs_lookup(path, &f)) && !store_file(path, 0, 0))
            return -1;
        memcpy(p->handles[i].path, path, length(path, 513) + 1);
        p->handles[i].append = value == 1;
        p->handles[i].pos = 0;
        return i + 3;
    }
    case 41: {
        unsigned index = value >> 48, mask = (value >> 32) & 65535;
        if (!mask || mask > 3 || index >= FD_LIMIT || (index < 3 && (p->closed & (1u << index))))
            return -1;
        u64 start = clock_ticks(), duration = clock_hz() * (u32)value / 1000;
        for (;;) {
            unsigned ready = 0;
            if (index < 3 && (p->null_streams & (1u << index))) ready = index ? 2 : 1;
            else if (index == 0)
                ready =
                    p->in ? pipe_ready(p->in, 0) : !!(p->input || p->background || console_ready());
            else if (index < 3)
                ready = index == 1 && p->out ? pipe_ready(p->out, 1) : 2;
            else if (p->handles[index-3].pipe)
                ready = pipe_ready(p->handles[index-3].pipe, p->handles[index-3].writer);
            else if (p->handles[index - 3].local)
                ready = local_ready(p->handles[index - 3].local);
            else if (p->handles[index - 3].shared)
                ready = 3;
            else if (p->handles[index - 3].socket)
                ready = socket_ready(p->handles[index - 3].socket - 1);
            else if (p->handles[index - 3].file.data)
                ready = 1;
            else if (p->handles[index - 3].path[0])
                ready = 2;
            else
                return -1;
            if (task_stopped() || (ready & mask) || ((u32)value != 0xffffffff && clock_ticks() - start >= duration))
                return ready & mask;
            schedule();
        }
    }
    case 39:
        if (fd >= 3 && fd < FD_LIMIT && n <= 4096 && p->handles[fd - 3].pipe)
            return p->handles[fd-3].writer ? pipe_write(p->handles[fd-3].pipe,(u8 *)mail,n) : -1;
        if (n > 4096)
            return -1;
        if (fd == 1 || fd == 2)
            return stream_write(p, fd, (const u8 *)mail, n, 0);
        if (fd < 3 || fd >= FD_LIMIT)
            return -1;
        if (p->handles[fd - 3].socket) {
            long sent;
            while ((sent = socket_io(p->handles[fd - 3].socket - 1, SOCKET_WRITE, (u8 *)mail, n)) == -2 && !task_stopped())
                schedule();
            return sent;
        }
        fd -= 3;
        if (p->handles[fd].file.data || !p->handles[fd].path[0] || !vfs_lookup(p->handles[fd].path, &f) || f.size > NATIVE_FILE_LIMIT)
            return -1;
        unsigned off = p->handles[fd].append ? f.size : p->handles[fd].pos;
        if (off > NATIVE_FILE_LIMIT - n)
            return -1;
        int fast = scratch_write(p->handles[fd].path, off, mail, n);
        if (fast >= 0) {
            if (!fast)
                return -1;
            p->handles[fd].pos = off + n;
            return n;
        }
        unsigned size = f.size > off + n ? f.size : off + n;
        u8 *tmp = file_buffer(size);
        if (!tmp) return -1;
        memcpy(tmp, f.data, f.size);
        if (off > f.size)
            memset(tmp + f.size, 0, off - f.size);
        memcpy(tmp + off, mail, n);
        int saved = store_file(p->handles[fd].path, tmp, size);
        release_buffer(tmp, size);
        if (!saved)
            return -1;
        p->handles[fd].pos = off + n;
        return n;
    case 7:
    case 8:
        if (length(mail, 513) > 512 || !resolve(p->cwd, mail, path) || !vfs_lookup(path, &f))
            return -3;
        if (op == 7) {
            long result = f.size == 0xffffffff ? -2 : proc_path(path) ? 0 : (long)f.size;
            if (value) {
                if (f.size != 0xffffffff)
                    return -4;
                result = vfs_entry(path, value - 1, mail + 513);
                if (result == -1)
                    return -1;
                char child[513];
                if (!resolve(path, mail + 513, child) || !vfs_lookup(child, &f))
                    return -3;
            }
            memcpy(mail + 1026, &f.birth, 4);
            memcpy(mail + 1030, &f.modified, 4);
            return result;
        }
        if (f.size == 0xffffffff)
            return -1;
        for (unsigned i = 0; i < HANDLE_COUNT; i++)
            if (!handle_used(p, i)) {
                u8 *bytes = file_buffer(f.size);
                if (!bytes)
                    return -1;
                memcpy(bytes, f.data, f.size);
                p->handles[i].file = (struct file){bytes, f.size};
                memcpy(p->handles[i].path, path, length(path, 513) + 1);
                p->handles[i].pos = 0;
                return i + 3;
            }
        return -1;
    case 9:
        if (fd >= 3 && fd < FD_LIMIT && n <= 4096 && p->handles[fd - 3].pipe)
            return !p->handles[fd-3].writer ? pipe_read(p->handles[fd-3].pipe,(u8 *)mail,n) : -1;
        if (fd == 0 && n <= 4096) {
            long count = stream_read(p, (u8 *)mail, n);
            return count == 1 && !p->input && !p->in && mail[0] == 4 ? 0 : count;
        }
        if (fd >= 3 && fd < FD_LIMIT && n <= 4096 && p->handles[fd - 3].socket) {
            long got;
            while ((got = socket_io(p->handles[fd - 3].socket - 1, SOCKET_READ, (u8 *)mail, n)) == -2 && !task_stopped())
                schedule();
            return got;
        }
        if (n <= 4096 && fd >= 3 && fd < FD_LIMIT && p->handles[fd - 3].path[0] && !p->handles[fd - 3].file.data) {
            if (!vfs_lookup(p->handles[fd-3].path, &f) || f.size == 0xffffffff) return -1;
            unsigned pos = p->handles[fd-3].pos;
            if (pos >= f.size) return 0;
            if (n > f.size-pos) n = f.size-pos;
            memcpy(mail, f.data+pos, n);
            p->handles[fd-3].pos += n;
            return n;
        }
        if (n > 4096 || fd < 3 || fd >= FD_LIMIT || !p->handles[fd - 3].file.data)
            return -1;
        fd -= 3;
        f = p->handles[fd].file;
        if (p->handles[fd].pos >= f.size) return 0;
        if (n > f.size - p->handles[fd].pos)
            n = f.size - p->handles[fd].pos;
        memcpy(mail, f.data + p->handles[fd].pos, n);
        p->handles[fd].pos += n;
        return n;
    case 10:
        if (value < 3) {
            if (p->closed & (1u << value))
                return -1;
            close_stream(p, value);
            return 0;
        }
        if (value < 3 || value >= FD_LIMIT || !handle_used(p, value - 3))
            return -1;
        close_handle(p, value - 3);
        return 0;
    case 13:
        return geometry() | (p->nano && canvas_size() ? 1UL << 33 : 0);
    case 33:
        return p->nano && canvas_size() ? editor_input() : -1;
    case 34:
        if (p->canvas && !p->nano) {
            if (!value || value > 4096) return -1;
            clipboard_copy((const u8 *)MAIL, value);
            return 0;
        }
        /* Fall through for the Nano document clipboard. */
    case 35: {
        if (op == 35 && p->canvas && !p->nano) {
            if (value == 1) { clipboard_cancel(); return 0; }
            return value == 0 ? clipboard_take_text((u8 *)MAIL) : -1;
        }

        if (!p->nano || !canvas_size())
            return -1;
        if (op == 34) {
            if (value > (1UL << 20))
                return -1;
            clipboard_copy((const void *)ARENA, value);
            return 0;
        }
        return clipboard_document((void *)ARENA);
    }
    case 14:
        return console_read(value > 1000 ? 1000 : value);
    case 19: {
        if (value == 2)
            return p->canvas && !p->background && process_pane() == focused_pane() && popup_tab_input()
                       ? (long)canvas_navigation_keys() : -1;
        u64 size = canvas_size();
        unsigned mode = value ? CANVAS_RESIZABLE : CANVAS_FIXED;
        if (!popup_tab_input() || value > 1 || (p->canvas && p->canvas != mode) ||
            p->background || p->out || p->capture || p->in || p->input || !size ||
            grow_heap(GRAPHICS_ARENA, ((size & 65535) * (size >> 16) + 1024) * 4) < 0)
            return 0;
        if (!p->canvas && value) {
            for (unsigned i = 0; i < BANKS; i++)
                if (processes[i].pane == p->pane && processes[i].canvas)
                    return 0;
        }
        if (!p->canvas && process_pane() == focused_pane()) close_edit_menu();
        p->canvas = mode;
        return size | (value ? (u64)canvas_generation() << 32 : 0);
    }
    case 20:
        return p->canvas && !!value == (p->canvas == CANVAS_RESIZABLE)
                   ? present((const u64 *)MAIL, ARENA, heaps[bank][GRAPHICS_ARENA].count * BLOCK, value)
                   : -1;
    case 21:
        if (!p->canvas) return -1;
        p->clipboard_options = value & 5;
        return canvas_pointer(value);
    case 36:
        return *(volatile u32 *)0x09010000;
    case 43: {
        if (value > 3600000)
            return -1;
        u64 start = clock_ticks(), wait = clock_hz() * value / 1000;
        while (!task_stopped() && clock_ticks() - start < wait)
            idle_slice();
        return 0;
    }
    case 18:
        return process_request(p, (u32)value, value >> 32);
    case 22:
    case 31:
        if (length(mail, 513) > 512 || !resolve(p->cwd, mail, path) ||
            (op == 31 && vfs_lookup(path, &f)))
            return -1;
        return save_output(path, value);
    case 49: {
        int r = -1, w = -1;
        for (int i = 0; i < HANDLE_COUNT; ++i) {
            if (handle_used(p, i)) continue;
            if (r < 0) r=i; else { w=i; break; }
        }
        if (w < 0) return -1;
        struct pipe *channel = pipe_create();
        if (!channel) return -1;
        p->handles[r].pipe = p->handles[w].pipe = channel;
        p->handles[r].writer = 0; p->handles[w].writer = 1;
        return ((long)(r+3) << 32) | (w+3);
    }
    case 50: {
        unsigned size = value >> 32;
        const char *data = (void *)MAIL;
        if (size < 4 || size > 32768 || data[size-1]) return -1;
        int slot = task_slot();
        if (slot < 0) return -1;
        struct process *child = &processes[slot];
        memset(child, 0, sizeof(*child));
        child->agent_child = p->agent;
        inherit_scope(child,p);
        if (value & (1UL << 24)) {
            unsigned len = (u8)data[0] | ((unsigned)(u8)data[1] << 8);
            if (len > sizeof(child->environment) || len+2 >= size || (len && data[1+len])) return -1;
            memcpy(child->environment, data+2, len);
            child->env_result = len+1;
            data += 2+len;
            size -= 2+len;
        }
        if (!resolve(p->cwd, data, child->cwd) || !vfs_lookup(child->cwd,&f) || f.size != 0xffffffff) return -1;
        unsigned offset = length(data,size)+1;
        if (offset >= size || !resolve(child->cwd, data+offset, child->executable) ||
            !vfs_lookup(child->executable,&f) || f.size == 0xffffffff) return -1;
        offset += length(data+offset,size-offset)+1;
        if (offset >= size || size-offset > sizeof(child->start_args)) return -1;
        memcpy(child->start_args,data+offset,size-offset);
        child->start_used = size-offset;
        while (offset < size) {
            if (++child->start_argc > 512) return -1;
            offset += length(data+offset,size-offset)+1;
        }
        struct pipe *streams[3] = {p->in,p->out,p->err};
        child->closed = p->closed;
        child->null_streams = p->null_streams;
        for (unsigned i=0;i<3;i++) {
            unsigned raw = (value >> (i*8)) & 255;
            if (raw == i) continue;
            child->null_streams &= ~(1u << i);
            if (raw == 255) {
                child->null_streams |= 1u << i;
                child->closed &= ~(1u << i);
                streams[i] = 0;
                continue;
            }
            if (raw == 1 && i == 2) {streams[2]=streams[1]; child->null_streams |= (child->null_streams & 2) << 1; continue;}
            if (raw < 3 || raw >= FD_LIMIT || !p->handles[raw - 3].pipe ||
                p->handles[raw - 3].writer != !!i)
                return -1;
            streams[i] = p->handles[raw-3].pipe;
            child->closed &= ~(1u << i);
        }
        child->in=streams[0]; child->out=streams[1]; child->err=streams[2];
        for (unsigned i=0;i<3;i++) pipe_ref(streams[i],!!i,1);
        if (!child->in && p->input) {child->input=p->input; child->input->references++;}
        if (!child->out && p->capture) {child->capture=p->capture; child->capture->references++;}
        child->pane=p->pane; child->background=p->background;
        return task_create(slot,0);
    }
    case 51: return task_wait_mode((u32)value, value >> 32);
    case 48:
        return length(mail, 254) < 254 ? socket_resolve(mail) : -1;
    case 47: {
        fd &= 0x7fffffff;
        if (fd < 3 || fd >= FD_LIMIT)
            return -1;
        if (!p->handles[fd-3].path[0] || ((value >> 63) && p->handles[fd-3].file.data)) return -1;
        if (!(value >> 63)) { p->handles[fd-3].pos = n; return 0; }
        const char *name = p->handles[fd-3].path;
        if (n > NATIVE_FILE_LIMIT || !vfs_lookup(name, &f) || f.size == 0xffffffff) return -1;
        if (n <= f.size) return store_file(name, f.data, n) ? 0 : -1;
        u8 *data = file_buffer(n);
        if (!data) return -1;
        memcpy(data, f.data, f.size);
        memset(data + f.size, 0, n - f.size);
        int ok = store_file(name, data, n);
        release_buffer(data, n);
        return ok ? 0 : -1;
    }
    case 46: {
        struct frame_map *heap = &heaps[bank][LIBC_ARENA];
        if (!value || value % BLOCK || value / BLOCK > heap->count || heap->sealed)
            return -1;
        heap->sealed = value / BLOCK;
        map_heap(bank);
        sync_image();
        return 0;
    }
    case 57: task_own_interrupt_key(); return 0;
    case 56:
        if (value == ~0UL) { p->env_result = 0; return 0; }
        if (value > sizeof(p->environment) || (value && ((char *)MAIL)[value-1])) return -1;
        memcpy(p->environment,(char *)MAIL,value);
        p->env_result = value+1;
        return 0;
    case 55: return task_stop_child(value);
    case 54: return task_query(value);
    case 53:
        if (p->env_result) memcpy((void *)MAIL,p->environment,p->env_result-1);
        return p->env_result;
    case 52: return grow_heap(EXTENDED_ARENA, value);
    case 37:
        if (value == ~0UL)
            return build_volume ? 320 * BLOCK : 64 * BLOCK;
        if (value >> 40 == 1)
            return grow_heap(WIDE_ARENA, value - (1UL << 40));
        return grow_heap(LIBC_ARENA, value);
    case 23:
        if (length(mail, 513) > 512 || !resolve(p->cwd, mail, path))
            return -1;
        reply(mail, path);
        return 0;
    case 24:
        if (value == 2) return !p->agent && !(p->null_streams & 1) && !(p->closed & 1) && !p->in && !p->input;
        return !p->out && !p->capture;
    default:
        return -1;
    }
}

static int load_elf(struct file f, u64 code_end, u64 data_end) {
    if (!magic(f, "\177ELF\2\1\1", 7) || f.size < 64 || le32(f.data + 16) != (183u << 16 | 2) ||
        le32(f.data + 20) != 1 || le64(f.data + 24) != USER || f.data[54] != 56 || f.data[55])
        return 0;
    u64 off = le64(f.data + 32);
    unsigned count = f.data[56] + 256u * f.data[57];
    if (off > f.size || count > (f.size - off) / 56)
        return 0;
    int entry = 0;
    for (unsigned i = 0; i < count; i++) {
        const u8 *ph = f.data + off + i * 56;
        if (le32(ph) != 1)
            continue;
        u64 base = le64(ph + 16), start = le64(ph + 8), size = le64(ph + 32), mem = le64(ph + 40);
        int code = le32(ph + 4) == 5;
        if (!code && le32(ph + 4) != 6)
            return 0;
        u64 lo = code ? USER : USER + BLOCK, hi = code ? code_end : data_end;
        if (base >= arena_base[PROGRAM_ARENA] && base < 3 * GIB) {
            u64 where = base - arena_base[PROGRAM_ARENA];
            if (size > mem || start > f.size || size > f.size - start || where > 256 * BLOCK ||
                mem > 256 * BLOCK - where || (where % BLOCK) || !mem ||
                (code ? where != 0 || heaps[bank][PROGRAM_ARENA].count || !size
                      : !heaps[bank][PROGRAM_ARENA].sealed ||
                            where != heaps[bank][PROGRAM_ARENA].sealed * BLOCK) ||
                grow_heap(PROGRAM_ARENA, where + mem) < 0)
                return 0;
            struct frame_map *h = &heaps[bank][PROGRAM_ARENA];
            for (u64 pos = 0; pos < size;) {
                u64 n = size - pos < BLOCK ? size - pos : BLOCK;
                memcpy((void *)frame_address(h->frames[(where + pos) / BLOCK]),
                       f.data + start + pos, n);
                pos += n;
            }
            if (code)
                h->sealed = h->count;
            map_heap(bank);
            continue;
        }
        if (base < lo || base > hi || mem > hi - base || size > mem || start > f.size ||
            size > f.size - start)
            return 0;
        if (base == USER && code && size >= 4)
            entry = 1;
        memcpy(physical() + (base - USER), f.data + start, size);
    }
    return entry;
}

static long start_desktop_child(struct process *p, int action, const char *request, int *child_slot) {
    const char *image = action == -3   ? "/bin/text-editor"
                        : (action == -2 || action == -5) ? request
                        : action == -1 ? "/bin/nano"
                        : (action == 0 || action == -4) ? "/bin/bash"
                                       : 0;
    if (!image || !resolve(p->cwd, image, p->path))
        return -1;
    struct file f;
    if (!vfs_lookup(p->path, &f))
        return -1;
    if ((action == -2 || action == -3) && !magic(f, "LXE1", 4) && !equal(p->path, "/bin/links"))
        return -1;
    char *arg = add_text(p->args, p->path) + 1;
    p->argc = 1;
    if (action == -1 || action == -3 || equal(p->path, "/bin/links")) {
        arg = add_text(arg, action == -2 ? "-g" : request) + 1;
        p->argc++;
    }
    if (action == -4) {
        arg = add_text(arg, "--launch") + 1;
        arg = add_text(arg, request) + 1;
        p->argc += 2;
    }
    p->used = arg - p->args;
    int slot = task_slot();
    int popup = action != -2 && action != -3 && action != -5;
    u64 canvas = canvas_size();
    unsigned tw = ((unsigned *)MAIL)[1];
    if (slot < 0 || (popup && (!readable(ARENA, (canvas & 65535) * 44 * 4UL) ||
                               !popup_open(slot, tw, (const u32 *)ARENA, action == -1))))
        return -1;
    if (action == 0 || action == -4) popup_mark_tab(slot);
    long pid = spawn(p, 8);
    if (pid < 0) {
        if (popup) popup_hide(slot, 0);
        return -1;
    }
    if (action == -1) processes[slot].start_flags = 16;
    *child_slot = slot;
    return pid;
}

static void remove_tab(struct process *p, unsigned index) {
    popup_hide(p->sessions[index].slot, 0);
    p->windows--;
    memmove(&p->sessions[index], &p->sessions[index + 1],
            (p->windows - index) * sizeof(p->sessions[0]));
}

static int add_tab(struct process *p, int previous, const char *command) {
    if (p->windows >= TERMINAL_TABS) return -1;
    int slot;
    long pid = start_desktop_child(p, command ? -4 : 0, command ? command : "", &slot);
    if (pid < 0) return -1;
    if (previous >= 0) popup_position(slot, previous);
    unsigned index = p->windows++;
    p->sessions[index].slot = slot;
    p->sessions[index].pid = pid;
    return index;
}

static long terminal_tabs(struct process *p, int action, const char *request) {
    for (unsigned i = 0; i < p->windows;) {
        if (task_wait_mode(p->sessions[i].pid, 1) == -2) i++;
        else remove_tab(p, i);
    }
    int active;
    if (!action || action == -4) {
        active = add_tab(p, -1, action == -4 ? request : 0);
    } else if (p->windows) {
        active = action > (int)p->windows ? p->windows - 1 : action - 1;
        if (!popup_show(p->sessions[active].slot)) return -1;
    } else return -1;
    if (active < 0) return -1;
    popup_tab_bar(active, p->windows);
    for (;;) {
        display_idle_pointer();
        int click = popup_tab_action();
        int slot = p->sessions[active].slot;
        u64 pid = p->sessions[active].pid;
        long status = task_wait_mode(pid, 1);
        if (status != -2 || click == TAB_CLOSE) {
            if (status == -2) {
                task_stop_child(pid);
                task_wait(pid);
            }
            popup_hide(slot, 1);
            if (p->windows > 1) {
                unsigned next = active == (int)p->windows - 1 ? active - 1 : active + 1;
                popup_position(p->sessions[next].slot, slot);
            }
            remove_tab(p, active);
            if (!p->windows) return 0;
            if (active >= (int)p->windows) active = p->windows - 1;
            if (!popup_show(p->sessions[active].slot)) return -1;
            popup_tab_bar(active, p->windows);
        } else if (click == TAB_HIDE) {
            popup_hide(slot, 1);
            return 0;
        } else if (click == TAB_NEW || (click > 0 && click != active + 1)) {
            popup_hide(slot, 1);
            if (click == TAB_NEW) {
                int new_tab = add_tab(p, slot, 0);
                if (new_tab >= 0) {
                    active = new_tab;
                } else if (!popup_show(slot)) return -1;
            } else {
                active = click - 1;
                int next = p->sessions[active].slot;
                popup_position(next, slot);
                if (!popup_show(next)) return -1;
            }
            popup_tab_bar(active, p->windows);
        } else {
            unsigned count = p->windows;
            for (unsigned i = 0; i < p->windows;) {
                if ((int)i == active || task_wait_mode(p->sessions[i].pid, 1) == -2) {
                    i++;
                } else {
                    remove_tab(p, i);
                    if ((int)i < active) active--;
                }
            }
            if (count != p->windows) popup_tab_bar(active, p->windows);
        }
        idle_slice();
    }
}

/* The desktop yields with a mailbox action after saving its EL0 continuation. */
static long desktop_child(struct process *p) {
    int action = *(int *)MAIL;
    const char *request = (const char *)(MAIL + 8);
    if (length(request, 513) > 512) return -1;
    if (action >= 0 || action == -4) return terminal_tabs(p, action, request);
    int slot;
    long pid = start_desktop_child(p, action, request, &slot);
    if (pid < 0) return -1;
    long status = task_wait(pid);
    if (action == -1) popup_hide(slot, 0);
    return status;
}
#include "agent-service.h"

static long launch(const char *name, const char *args, unsigned n, unsigned argc, unsigned flags) {
    struct file f;
    if (!disk_can_execute(name) || !vfs_lookup(name, &f) || !argc || n > 32768)
        return -1;
    for (unsigned depth = 0; magic(f, "LXalias1\n", 9); depth++) {
        if (depth == 8 || f.size <= 9 || f.size > 521)
            return -1;
        char target[513];
        memcpy(target, f.data + 9, f.size - 9);
        target[f.size - 9] = 0;
        if (!canonical_path(target) || !disk_can_execute(target) || !vfs_lookup(target, &f))
            return -1;
    }
    struct process *active=&processes[bank];
    if (equal(name,"/lib/agent-command") && magic(f,"LXcmd1\nagent-command",20) && f.size==20) {
        if (!active->agent_child || argc<2 || !active->in || !active->out ||
            equal(active->cwd,"/") || under(active->cwd,"/bin") || under(active->cwd,"/lib") ||
            under(active->cwd,"/src") || under(active->cwd,"/boot") || under(active->cwd,"/agent-policy") ||
            under(active->cwd,"/libexec") || proc_path(active->cwd)) return 1;
        unsigned skip=length(args,n)+1;
        if (skip>=n) return 1;
        const char *command = args + skip;
        unsigned command_size = n - skip;
        static const char desktop_check[] = "/bin/python3\0-I\0/src/agent/desktop.py\0check";
        static const char reuse[] = "--reuse";
        int check = ((argc == 5 && command_size == sizeof(desktop_check)) ||
                     (argc == 6 && command_size == sizeof(desktop_check) + sizeof(reuse) &&
                      !memcmp(command + sizeof(desktop_check), reuse, sizeof(reuse)))) &&
                    !memcmp(command, desktop_check, sizeof(desktop_check));
        active->agent_child=0;
        memcpy(active->scope,active->cwd,length(active->cwd,513)+1);
        // Desktop checks compile, run the regression suite, and save the candidate.
        active->command_deadline=clock_ticks()+(check ? 60 : 5)*clock_hz();
        char program[513];
        if (!resolve(active->cwd,args+skip,program)) return 1;
        return launch(program,args+skip,n-skip,argc-1,0);
    }
    if (active->scope[0] && magic(f,"LXcmd1\n",7) &&
        !(magic(f,"LXcmd1\ngit",10) && f.size==10)) return 1;
    if (magic(f, "LXcmd1\nshutdown", 15) && f.size == 15 && argc == 1) {
        if (id_uid(task_active())) return 1;
        if (!disk_checkpoint()) {
            serial_text("Build disk checkpoint failed\n");
            return 1;
        }
        serial_text("Shutting down LixinOS...\n");
        power_off();
        return -1;
    }
    struct process *self = &processes[bank];
    if (equal(name,"/lib/agent-io") && magic(f, "LXcmd1\nagent-io", 15) && f.size == 15) {
        if (!self->agent_child || !self->in || !self->out || argc != 2) return 1;
        return agent_io(self, args + length(args,n) + 1);
    }
    if (equal(name,"/bin/agent") && magic(f, "LXcmd1\nagent", 12) && f.size == 12) {
        static const char prefix[] = "python3\0-I\0/src/agent/native.py";
        unsigned skip = length(args,n) + 1;
        if (skip > n || sizeof(prefix) + n - skip > sizeof(self->start_args)
            || !lookup("/bin/python3", &f)) return 1;
        memmove(self->start_args + sizeof(prefix), args + skip, n - skip);
        memcpy(self->start_args, prefix, sizeof(prefix));
        n = sizeof(prefix) + n - skip;
        argc += 2;
        args = self->start_args;
        self->agent = 1;
    }
    char packed[560], git_args[1024], git_root[513];
    const char *mode = 0;
    if (magic(f, "LXcmd1\n", 7)) {
        const char *modes[] = {"kilo", "nano", "paint", "htop"};
        for (unsigned i = 0; i < 4; i++)
            if (f.size == 7 + strlen(modes[i]) && !memcmp(f.data + 7, modes[i], f.size - 7))
                mode = modes[i];
    }
    unsigned header = 0;
    if (mode) {
        if (self->background || self->in || self->input || self->out || self->capture)
            return -1;
        self->foreground = 1;
        self->nano = equal(mode, "nano") && self->start_flags == 16;
        if (!equal(mode, "htop")) {
            const char *path = argc == 2                           ? args + length(args, n) + 1
                               : argc == 1 && equal(mode, "paint") ? "drawing.svg"
                                                                   : 0;
            if (!path || (equal(mode, "paint") && !canvas_size()) ||
                !resolve(self->cwd, path, self->edit_path))
                return -1;
            struct file source;
            if (grow_heap(GRAPHICS_ARENA, (1UL << 20) + 1) < 0)
                return -1;
            if (vfs_lookup(self->edit_path, &source)) {
                if (source.size > EDITOR_FILE_LIMIT || !valid_text(source.data, source.size))
                    return -1;
                u8 *copy = file_buffer(source.size);
                if (!copy)
                    return -1;
                memcpy(copy, source.data, source.size);
                self->original = (struct file){copy, source.size};
                memcpy((void *)ARENA, copy, source.size);
            }
        } else if (argc != 1)
            return -1;
        char override[32];
        add_text(add_text(override, "/libexec/"), mode);
        if (!vfs_lookup(override, &f) && !vfs_lookup("/lib/foreground.elf", &f))
            return -1;
        char *path = add_text(packed, mode) + 1, *size = add_text(path, self->edit_path) + 1;
        header = size - packed;
        n = (add_number(size, self->original.size) + 1) - packed;
        args = packed;
        argc = 3;
    }
    int git = magic(f, "LXcmd1\ngit", 10) && f.size == 10;
    if (git) {
        if (!vfs_lookup("/lib/git.elf", &f))
            return -1;
        unsigned first = length(args, n) + 1, cwd = length(self->cwd, 513);
        if (first >= n || n - first >= 1024 || cwd > 502)
            return -1;
        memcpy(git_root, self->cwd, cwd + 1);
        if (!equal(args + first, "init"))
            for (;;) {
                char head[513];
                struct file entry;
                unsigned prefix = cwd == 1 ? 0 : cwd;
                memcpy(head, git_root, prefix);
                memcpy(head + prefix, "/.git/HEAD", 11);
                if (vfs_lookup(head, &entry))
                    break;
                if (cwd == 1)
                    return -1;
                while (cwd > 1 && git_root[cwd - 1] != '/')
                    cwd--;
                if (cwd > 1)
                    cwd--;
                git_root[cwd] = 0;
            }
        unsigned prefix = cwd == 1 ? 0 : cwd;
        memcpy(self->git_meta, git_root, prefix);
        memcpy(self->git_meta + prefix, "/.git", 6);
        if (equal(args + first, "add")) {
            char *out = add_text(git_args, "git") + 1;
            out = add_text(out, "add") + 1;
            unsigned pos = first + 4;
            if (pos < n && equal(args + pos, "-A")) {
                pos += 3;
                if (pos == n)
                    out = add_text(out, ".") + 1;
            }
            while (pos < n) {
                char absolute[513];
                if (!resolve(self->cwd, args + pos, absolute))
                    return -1;
                const char *relative;
                if (equal(absolute, git_root))
                    relative = ".";
                else {
                    if (memcmp(absolute, git_root, prefix) || absolute[prefix] != '/')
                        return -1;
                    relative = absolute + prefix + 1;
                }
                for (const char *part = relative; *part;) {
                    const char *end = part;
                    while (*end && *end != '/')
                        end++;
                    if (end - part == 4 && part[0] == '.' && (part[1] | 32) == 'g' &&
                        (part[2] | 32) == 'i' && (part[3] | 32) == 't')
                        return -1;
                    part = *end ? end + 1 : end;
                }
                if (strlen(relative) + 1 > sizeof(git_args) - (out - git_args))
                    return -1;
                out = add_text(out, relative) + 1;
                pos += length(args + pos, n - pos) + 1;
            }
            args = git_args;
            n = out - git_args;
        }
        if (!batch_begin(bank))
            return -1;
    }
    if (f.size != 0xffffffff && musl_program(f.data,f.size)) {
        if (!vfs_lookup("/bin/ld-musl", &f) || f.size == 0xffffffff) return -1;
    }
    u8 *image = physical(), *argument = image + (MAIL - USER);
    memset(image, 0, 2 * BLOCK);
    u64 a, b, c, d;
    if (magic(f, "LXE1", 4) && f.size >= 16) {
        struct file runtime;
        if (!vfs_lookup("/lib/lxe-runtime.elf", &runtime) || !load_elf(runtime, CODE, DATA))
            return -1;
        u32 entry = le32(f.data + 4), code = le32(f.data + 8), data = le32(f.data + 12);
        if ((entry & 3) || code < 4 || entry > code - 4 || code > USER + BLOCK - CODE ||
            data > UTILITY - DATA || 16UL + code + data != f.size)
            return -1;
        memcpy(image + (CODE - USER), f.data + 16, code);
        memcpy(image + (DATA - USER), f.data + 16 + code, data);
        memcpy(argument, args, n);
        a = CODE + entry;
        b = argc;
        c = MAIL;
        d = flags;
    } else if (mode) {
        if (!load_elf(f, USER + BLOCK, UTILITY))
            return -1;
        memcpy(argument, args, n);
        a = MAIL;
        b = MAIL + header;
        c = n - header - 1;
        d = 0;
    } else {
        if (!load_elf(f, USER + BLOCK, UTILITY))
            return -1;
        const char *working = git ? git_root : processes[bank].cwd;
        u32 cwd = length(working, 513) + 1;
        u32 first = length(args, n) + 1;
        if (first > n || cwd + n - first > 32768)
            return -1;
        memcpy(argument, working, cwd);
        memcpy(argument + cwd, args + first, n - first);
        a = MAIL;
        b = MAIL + cwd;
        c = n - first;
        d = 0;
    }
    sync_image();
    long status = kt_end_group(enter_el0(a, b, c, d));
    while (status == 256) {
        struct process *parent = &processes[bank];
        if (parent->agent_child && equal(name, "/bin/agent-run") && !parent->argc) {
            status = kt_end_group(enter_el0(0,0,0,65536));
            continue;
        }
        if (parent->canvas && !parent->argc) {
            long child = desktop_child(parent);
            *(int *)MAIL = parent->windows;
            add_text((char *)(MAIL + 8), child ? "Application failed" : "");
            status = kt_end_group(enter_el0(0, 0, 0, 65536));
            continue;
        }
        long pid = spawn(parent, 8);
        parent->status = pid < 0 ? -1 : task_wait(pid);
        status = kt_end_group(enter_el0(0, 0, 0, 0));
    }
    if (self->agent) display_agent_bar(0,"");
    return git ? git_finish(bank, status) : status;
}

static long spawn(struct process *parent, unsigned flags) {
    int slot = task_slot();
    if (slot < 0 || !parent->argc || flags & ~15u)
        return -1;
    struct process *p = &processes[slot];
    memset(p, 0, sizeof(*p));
    inherit_scope(p,parent);
    memcpy(p->cwd, parent->cwd, length(parent->cwd, 513) + 1);
    memcpy(p->executable, parent->path, length(parent->path, 513) + 1);
    memcpy(p->start_args, parent->args, parent->used);
    p->start_argc = parent->argc;
    p->start_used = parent->used;
    p->pane = parent->pane;
    p->background = parent->background || (flags & 4);
    struct file f;
    if (!vfs_lookup(parent->path, &f) || f.size == 0xffffffff)
        return -1;
    if (flags == 8) {
        p->closed = parent->closed;
        p->err = parent->err; pipe_ref(p->err,1,1);
        p->in = parent->in;
        pipe_ref(p->in, 0, 1);
        p->out = parent->out;
        pipe_ref(p->out, 1, 1);
    } else {
        if (parent->next_input[0]) {
            p->input = redirect_open(parent->next_input, 0, 0);
            if (!p->input)
                return -1;
        }
        if (parent->next_output[0]) {
            p->capture = redirect_open(parent->next_output, 1, parent->append);
            if (!p->capture)
                goto fail;
        }
        struct pipe *next = (flags & 2) ? pipe_create() : 0;
        if ((flags & 2) && !next)
            goto fail;
        if (p->input)
            pipe_ref(parent->pending_pipe, 0, -1);
        else
            p->in = parent->pending_pipe;
        if (p->capture)
            pipe_ref(next, 1, -1);
        else
            p->out = next;
        parent->pending_pipe = next;
        parent->next_output[0] = parent->next_input[0] = 0;
    }
    if (!p->in && !p->input && parent->input) {
        p->input = parent->input;
        p->input->references++;
    }
    if (!p->out && !p->capture && parent->capture) {
        p->capture = parent->capture;
        p->capture->references++;
    }
    parent->used = parent->argc = 0;
    long pid = task_create(slot, !!(flags & 4));
    if (pid > 0 && !p->background && !p->scope[0]) task_job(slot);
    return pid;
fail:
    redirect_close(p->capture, 0);
    redirect_close(p->input, 0);
    return -1;
}

long native_syscall(u64 value, u64 op) {
    long result;
    if (op==61) result=smp_clock_allowed() ? kt_call(value) : -1;
    else if (op==38 || op==43) result=syscall_service(value,op);
    else {
        kt_service_enter();
        result=syscall_service(value,op);
        kt_service_leave();
    }
    int stop = task_stopped();
    return stop == 137 ? -4094 : stop == 130 ? -4095 : result;
}

long run_task(unsigned slot) {
    struct process *p = &processes[slot];
    long status =
        launch(p->executable, p->start_args, p->start_used, p->start_argc, p->start_flags);
    while (p->windows) {
        unsigned i = --p->windows;
        int child = p->sessions[i].slot;
        task_stop_child(p->sessions[i].pid);
        task_wait(p->sessions[i].pid);
        popup_hide(child, 0);
    }
    batch_abort(slot);
    release_buffer(p->original.data, p->original.size);
    close_stream(p, 0);
    close_stream(p, 1);
    close_stream(p, 2);
    pipe_ref(p->pending_pipe, 0, -1);
    if (!redirect_close(p->capture, 1))
        status = 1;
    redirect_close(p->input, 0);
    for (unsigned i = 0; i < HANDLE_COUNT; i++)
        close_handle(p, i);
    record_release(p->record_owner);
    p->record_owner = 0;
    gpu_close(&p->gpu_context);
    audio_close(&p->audio_stream);
    shared_clear(slot);
    vm_publish(slot, 0);
    vm_sync();
    vm_destroy(&vm_spaces[slot]);
    for (unsigned kind = 0; kind < ARENAS; kind++) {
        struct frame_map *heap = &heaps[slot][kind];
        heap->sealed = 0;
        while (heap->count)
            heap_used[heap->frames[--heap->count]] = 0;
    }
    if (p->canvas) {
        if (process_pane() == focused_pane()) close_edit_menu();
        display_restore();
    }
    p->canvas = p->foreground = 0;
    return status;
}

static void fault_word(u64 value) {
    const char *digits="0123456789abcdef";
    char text[19];
    char *p=text+sizeof(text);
    *--p=0;
    do { *--p=digits[value&15]; value >>= 4; } while(value);
    *--p='x'; *--p='0';
    serial_text(p);
}
void report_user_fault(u64 syndrome, u64 pc, u64 address, u64 link) {
    serial_text("User fault: ESR="); fault_word(syndrome);
    serial_text(" PC="); fault_word(pc);
    serial_text(" FAR="); fault_word(address);
    serial_text(" LR="); fault_word(link); serial_text("\n");
}

void fatal_vector(void) {
    serial_text("Fatal kernel exception\n");
    power_off();
    for (;;) {
    }
}

void smp_native_root(void) {
    extern void smp_root(unsigned long table);
    smp_root((u64)cpu_tables[1].root);
}
static int mmu_ready;
void early_mmu_init(int high) {
    if (mmu_ready)
        return;
    for (unsigned c=0; c<2; c++) {
        struct cpu_tables *t=&cpu_tables[c];
        t->root[0] = 1 | (1 << 10) | 4 | PXN | UXN;
        t->root[1] = (u64)t->ram | 3;
        t->root[2] = (u64)t->ram_high | 3;
        t->root[3] = (u64)t->extended | 3;
        t->root[LX_SHARED_BASE / GIB] = shared_root(c);
        for (unsigned i = 0; i < 512; i++)
            t->ram[i] = (0x40000000UL + i * BLOCK) | NORMAL | UXN;
        for (unsigned i = 0; i < (NATIVE_RAM_BYTES - (1024UL << 20)) / BLOCK; i++)
            t->ram_high[i] = (0x80000000UL + i * BLOCK) | NORMAL | UXN;
        if (high) {
            for (unsigned i = 4; i < (BUILD_RAM + GIB) / GIB; i++)
                t->root[i] = i * GIB | NORMAL | PXN | UXN;
            for (unsigned i = 0; i < 4; i++)
                t->root[64 + i] = (u64)t->wide_tables[i] | 3;
        }
    }
    enable_mmu(cpu_tables[0].root, native_vectors, high);
    extern int normal_ram;
    normal_ram = 1;
    mmu_ready = 1;
}

int run_login(void) {
    early_mmu_init(build_volume);
    select_bank(0);
    tasks_init();
    proc_init();
    processes[0].cwd[0] = '/';
    add_text(processes[0].executable, split_mode() ? "init" : "/bin/sh");
    if (!split_mode())
        return launch("/bin/sh", "sh", 3, 1, ssh_mode() ? 6 : 2);
    struct process *parent = &processes[0];
    long child[2];
    for (unsigned pane = 0; pane < 2; pane++) {
        add_text(parent->path, "/bin/sh");
        add_text(parent->args, "sh");
        parent->argc = 1;
        parent->used = 3;
        int slot = task_slot();
        child[pane] = spawn(parent, 8);
        if (child[pane] < 0)
            return 1;
        processes[slot].pane = pane;
        processes[slot].start_flags = 2;
    }
    for (unsigned i = 0; i < 2; i++)
        task_wait(child[i]);
    return 0;
}
