// Load both images at distinct guest addresses and enter the interpreter with a Linux initial
// stack.
#include "runtime.h"
#include "mmu.h"
#include "predecode.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

int tracing, independent_processes;

void fail(const char *why) {
    CPU *cpu = thread_cpu();
    fprintf(stderr, "linux-run: %s (pc=%#llx)\n", why, (unsigned long long)cpu->cur_insn_pc);
    if (tracing) {
        u64 fp = cpu->x[29];
        fprintf(stderr, "linux-run: lr=%#lx", (unsigned long)cpu->x[30]);
        for (int i = 0; i < 20 && fp; ++i) {
            u64 frame[2];
            void *slot = guest_ptr(fp, sizeof frame, PROT_READ);
            if (!slot)
                break;
            memcpy(frame, slot, sizeof frame);
            fprintf(stderr, " %#lx", (unsigned long)frame[1]);
            if (frame[0] <= fp)
                break;
            fp = frame[0];
        }
        fputc('\n', stderr);
    }
    process_exit(126);
}
void exception_take(CPU *c, ExcKind kind, u64 esr, u64 far, u64 ret) {
    (void)c;
    (void)kind;
    (void)ret;
    fprintf(stderr, "linux-run: guest exception ESR=%#llx address=%#llx\n", (unsigned long long)esr,
            (unsigned long long)far);
    fail("guest stopped");
}
void cpu_raise_sync(CPU *c, u64 esr, u64 far) {
    exception_take(c, EXC_SYNC, esr, far, c->cur_insn_pc);
}

struct image {
    u64 entry, phdr;
    unsigned phnum;
};
static struct image load(const char *path, u64 base, u64 limit, int main_image) {
    struct backing *file = open_backing(path);
    if (!file) {
        perror(path);
        fail("cannot open ELF");
    }
    int fd = file->fd;
    Elf64_Ehdr h;
    if (read_segment(fd, &h, sizeof h, 0) || memcmp(h.e_ident, "\177ELF\2\1\1", 7) ||
        h.e_type != ET_DYN || h.e_machine != EM_AARCH64 || h.e_phentsize != sizeof(Elf64_Phdr) ||
        !h.e_phnum || h.e_phnum > 128 || h.e_phoff > file->size ||
        h.e_phnum * sizeof(Elf64_Phdr) > file->size - h.e_phoff)
        fail("expected a little-endian AArch64 PIE");
    Elf64_Phdr ph[128];
    if (read_segment(fd, ph, h.e_phnum * sizeof(*ph), h.e_phoff))
        fail("short program header table");
    // Large PIEs can extend past the loader's fixed address. Reserve their
    // entire image, including gaps, in the same arena that ld.so uses for DSOs.
    if (main_image) {
        u64 span = 0;
        for (unsigned i = 0; i < h.e_phnum; ++i) {
            const Elf64_Phdr *p = &ph[i];
            if (p->p_type != PT_LOAD || !p->p_memsz)
                continue;
            if (p->p_vaddr >= ARENA || p->p_memsz > ARENA - p->p_vaddr)
                fail("PIE exceeds guest address space");
            u64 end = p->p_vaddr + p->p_memsz;
            if (end > span)
                span = end;
        }
        if (span > limit - base) {
            base = map_linux(0, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, NULL, 0);
            if (base >= STACK_BASE)
                fail("cannot reserve large PIE");
            limit = base + span;
        }
    }
    struct image image = {0, 0, h.e_phnum};
    int interpreters = 0;
    for (unsigned i = 0; i < h.e_phnum; i++) {
        const Elf64_Phdr *p = ph + i;
        if (p->p_type == PT_INTERP) {
            char name[sizeof INTERP];
            if (++interpreters != 1 || p->p_filesz != sizeof name ||
                read_segment(fd, name, sizeof name, p->p_offset) ||
                memcmp(name, INTERP, sizeof name))
                fail("unsupported PT_INTERP");
        }
        if (p->p_type != PT_LOAD || !p->p_memsz)
            continue;
        if (p->p_vaddr >= limit - base || p->p_memsz > limit - base - p->p_vaddr ||
            p->p_filesz > p->p_memsz || p->p_offset > file->size ||
            p->p_filesz > file->size - p->p_offset || (p->p_offset % PAGE) != (p->p_vaddr % PAGE))
            fail("invalid PT_LOAD");
        u64 va = base + p->p_vaddr;
        int perms = (p->p_flags & PF_R ? PROT_READ : 0) | (p->p_flags & PF_W ? PROT_WRITE : 0) |
                    (p->p_flags & PF_X ? PROT_EXEC : 0);
        u64 prefix = va % PAGE;
        if (set_mapping(va - prefix, p->p_memsz + prefix, perms, file, p->p_offset - prefix,
                        p->p_filesz ? p->p_filesz + prefix : 0))
            fail("invalid ELF mapping");
        if ((perms & PROT_EXEC) && h.e_entry >= p->p_vaddr && h.e_entry - p->p_vaddr < p->p_filesz)
            image.entry = base + h.e_entry;
        if (h.e_phoff >= p->p_offset && h.e_phoff - p->p_offset <= p->p_filesz &&
            sizeof(*ph) * h.e_phnum <= p->p_filesz - (h.e_phoff - p->p_offset))
            image.phdr = va + (h.e_phoff - p->p_offset);
    }
    close_backing(file);
    if (!image.phdr || !image.entry || interpreters != main_image)
        fail("invalid entry, PHDR or interpreter");
    if (tracing)
        fprintf(stderr, "linux-run: mapped %s\n", path);
    return image;
}

static u64 push(u64 *sp, const void *data, size_t size) {
    if (size > *sp - STACK_BASE)
        fail("argument list exceeds stack");
    *sp -= size;
    memcpy(guest_ptr(*sp, size, PROT_WRITE), data, size);
    return *sp;
}
enum { MAX_VECTOR = 256, MAX_EXEC_BYTES = 128 * 1024 };
static const char *initial_executable;
static void load_program(CPU *cpu, const char *path, size_t argc, char *const argv[], size_t envc,
                         char *const env[]) {
    int direct = !strcmp(path, INTERP);
    struct image ld = load(INTERP, LD_BASE, BRK_BASE, 0);
    struct image app = direct ? ld : load(path, MAIN_BASE, LD_BASE, 1);
    page_update(STACK_BASE, ARENA, PROT_READ | PROT_WRITE, 0);
    u64 sp = ARENA, words[2 * MAX_VECTOR + 64], n = 0;
    words[n++] = argc;
    for (size_t i = 0; i < argc; i++)
        words[n++] = push(&sp, argv[i], strlen(argv[i]) + 1);
    words[n++] = 0;
    for (size_t i = 0; i < envc; i++)
        words[n++] = push(&sp, env[i], strlen(env[i]) + 1);
    words[n++] = 0;
    u64 executable = push(&sp, path, strlen(path) + 1);
    unsigned char entropy[16];
    if (getrandom(entropy, sizeof entropy, 0) != sizeof entropy)
        fail("guest entropy unavailable");
    u64 random_addr = push(&sp, entropy, sizeof entropy);
    u64 aux[] = {AT_PHDR,   app.phdr,
                 AT_PHENT,  sizeof(Elf64_Phdr),
                 AT_PHNUM,  app.phnum,
                 AT_BASE,   direct ? 0 : LD_BASE,
                 AT_ENTRY,  app.entry,
                 AT_PAGESZ, PAGE,
                 AT_UID,    namespace_id(0),
                 AT_EUID,   namespace_id(0),
                 AT_GID,    namespace_id(1),
                 AT_EGID,   namespace_id(1),
                 AT_SECURE, 0,
                 AT_HWCAP,  3,
                 AT_HWCAP2, 0,
                 AT_CLKTCK, 100,
                 AT_RANDOM, random_addr,
                 AT_EXECFN, executable,
                 AT_NULL,   0};
    memcpy(words + n, aux, sizeof aux);
    n += sizeof(aux) / sizeof(*aux);
    if (n * sizeof(u64) > sp - STACK_BASE)
        fail("argument list exceeds stack");
    sp = (sp - n * sizeof(u64)) & ~(u64)15;
    memcpy(guest_ptr(sp, n * sizeof(u64), PROT_WRITE), words, n * sizeof(u64));
    u64 ticks = cpu->icount;
    cpu_reset(cpu, ld.entry, 0);
    cpu->icount = ticks;
    *cpu_cur_sp(cpu) = sp;
}

// Copy the vectors while the old address space is still intact. Validate each
// page separately because the next Linux page may have different host backing.
static int copy_vector(u64 vector, char **out, size_t *count, char *strings, size_t *used) {
    *count = 0;
    if (!vector)
        return 0;
    for (size_t i = 0; i <= MAX_VECTOR; i++) {
        u64 addr;
        void *slot = guest_ptr(vector + i * sizeof addr, sizeof addr, PROT_READ);
        if (!slot)
            return -EFAULT;
        memcpy(&addr, slot, sizeof addr);
        if (!addr)
            return 0;
        if (i == MAX_VECTOR)
            return -E2BIG;
        out[i] = strings + *used;
        for (;;) {
            size_t room = MAX_EXEC_BYTES - *used;
            if (!room)
                return -E2BIG;
            size_t chunk = PAGE - addr % PAGE;
            if (chunk > room)
                chunk = room;
            const char *p = guest_ptr(addr, chunk, PROT_READ);
            if (!p)
                return -EFAULT;
            const char *end = memchr(p, 0, chunk);
            size_t n = end ? (size_t)(end - p) + 1 : chunk;
            memcpy(strings + *used, p, n);
            *used += n;
            addr += n;
            if (end)
                break;
        }
        ++*count;
    }
    return -E2BIG;
}
long exec_guest(CPU *cpu, const char *path, u64 argv, u64 envp) {
    if (strcmp(path, initial_executable) && strcmp(path, "/proc/self/exe"))
        return -ENOENT;
    if (guest_process->threads != 1)
        return -EAGAIN;
    char *strings = malloc(MAX_EXEC_BYTES);
    if (!strings)
        return -ENOMEM;
    char *args[MAX_VECTOR], *env[MAX_VECTOR];
    size_t argc, envc, used = 0;
    int rc = copy_vector(argv, args, &argc, strings, &used);
    if (!rc)
        rc = copy_vector(envp, env, &envc, strings, &used);
    struct guest_process fresh = {0};
    if (!rc)
        rc = memory_create(&fresh);
    if (!rc)
        rc = upper_create(&fresh);
    if (rc) {
        memory_destroy(&fresh);
        free(strings);
        return rc;
    }
    memory_destroy(guest_process);
    upper_destroy(guest_process);
    guest_process->memory = fresh.memory;
    guest_process->upper = fresh.upper;
    guest_process->vfork_active = 0;
    guest_process->sigreturn_pc = 0;
    files_on_exec(cpu);
    thread_set_tid(0);
    thread_exec_name(initial_executable);
    if (!argc)
        args[argc++] = "";
    load_program(cpu, initial_executable, argc, args, envc, env);
    free(strings);
    return 0;
}
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--trace")) {
        tracing = 1;
        argc--;
        argv++;
    }
    if (argc > 1 && !strcmp(argv[1], "--processes")) {
        independent_processes = 1;
        argc--;
        argv++;
    }
    const char *state = NULL;
    if (argc >= 4 && !strcmp(argv[1], "--state")) {
        state = argv[2];
        argv += 2;
        argc -= 2;
    }
    if (argc < 2 || argc > 65) {
        fputs("usage: linux-run [--trace] [--processes] [--state DIR] PIE [ARG ...]\n", stderr);
        return 2;
    }
    const char *name = strrchr(argv[1], '/');
    name = name ? name + 1 : argv[1];
    int chromium = !strcmp(name, "chromium-shell") || !strcmp(name, "content_shell");
    if (state)
        state_init(state, chromium ? "/home/chromium" : "/home/wechat");
    files_init();
    events_init();
    upper_init();
    mem_init();
    initial_executable = argv[1];
    allow_program_path(
        !strcmp(argv[1], INTERP) && argc == 4 && !strcmp(argv[2], "--list") ? argv[3] : argv[1],
        argv[1]);
    char *env[] = {chromium ? "HOME=/home/chromium" : "HOME=/home/wechat",
                   chromium ? "USER=chromium" : "USER=wechat",
                   chromium ? "LOGNAME=chromium" : "LOGNAME=wechat",
                   "TMPDIR=/tmp",
                   "XDG_RUNTIME_DIR=/run/user/1000",
                   "XDG_SESSION_TYPE=wayland",
                   "QT_QPA_PLATFORM=wayland",
                   "WAYLAND_DISPLAY=wayland-0",
                   "QT_QUICK_BACKEND=software",
                   "WAYLAND_DEBUG=client"};
    size_t envc = state ? sizeof env / sizeof *env - !getenv("WAYLAND_DEBUG") : 0;
    CPU *cpu = start_threads();
    thread_exec_name(initial_executable);
    load_program(cpu, argv[1], argc - 1, argv + 1, envc, env);
    interpret(cpu);
    return 126;
}
void interpret(CPU *cpu) {
    for (;;) {
        // A syscall or another guest CPU may have replaced the code mapping.
        tlb_flush_all();
        if (pd_run(cpu)) {
            long r = service(cpu);
            if (tracing && r < 0 && r >= -4095)
                fprintf(stderr, "linux-run: syscall %lu -> %ld\n", (unsigned long)cpu->x[8], r);
            cpu->x[0] = r;
            signal_checkpoint(cpu);
            cpu->icount++;
        }
        if (!(cpu->icount % PD_SLICE)) {
            thread_slice(cpu);
            signal_checkpoint(cpu);
        }
        if (cpu->stop || cpu->halted)
            fail("interpreter stopped");
    }
}

// The interpreter only runs EL0. System-mode exception returns are unsupported.
void cpu_unpack_spsr(CPU *c, u32 saved) {
    (void)c;
    (void)saved;
    fail("exception return outside EL0");
}
u64 gt_count(CPU *c, bool virtual_timer) {
    (void)virtual_timer;
    return c->icount;
}
