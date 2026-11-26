// Linux ABI translation for one application on the LixinOS file and socket services.
#include "resolver.h"
#include "../../Kernel/native/record-lock.h"
extern long lx_record_lock(int, unsigned, struct lx_record_request *);
#include "runtime.h"
extern int lx_lookup_ipv4(const char *, unsigned char *);
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

// Shared libraries are read-only guest files. Translate descriptor numbers so a
// Linux program cannot use descriptors belonging to its enclosing LixinOS process.

/* AArch64 Linux's rt_sigaction ABI uses a one-word kernel signal mask.
 * Guest dispositions belong to the emulated process, not the surrounding
 * musl program. Delivery to guest handler addresses needs a signal frame. */
struct guest_files {
    int std_closed[3], devices[GUEST_FDS];
    unsigned char cloexec[GUEST_FDS], ranges[GUEST_FDS];
    struct linux_sigaction actions[65];
    struct backing *libraries[GUEST_FDS];
    unsigned short sockets[GUEST_FDS];
    struct proc_handle *proc[GUEST_FDS];
    struct shm_handle *shm[GUEST_FDS];
    char paths[GUEST_FDS][513];
    int records[GUEST_FDS], flocks[GUEST_FDS], native[GUEST_FDS];
};
static struct guest_files initial_fds;
static struct guest_files *fd_table(void) {
    return guest_process->descriptors ? guest_process->descriptors : &initial_fds;
}
#define shm_files (fd_table()->shm)
#define dispositions (fd_table()->actions)
#define closed_stdio (fd_table()->std_closed)
#define files (fd_table()->libraries)
#define device (fd_table()->devices)
enum { RANDOM_DEVICE = 1, NULL_DEVICE };
#define families (fd_table()->sockets)
#define private_paths (fd_table()->paths)
#define record_locks (fd_table()->records)
#define flock_locks (fd_table()->flocks)
#define state_fds (fd_table()->native)

static long linux_sigaction(u64 signo, u64 action, u64 old, u64 mask_size) {
    if (!signo || signo >= 65 || mask_size != sizeof(u64))
        return -EINVAL;
    struct linux_sigaction next;
    if (action) {
        if (signo == SIGKILL || signo == SIGSTOP)
            return -EINVAL;
        void *in = guest_ptr(action, sizeof(next), PROT_READ);
        if (!in)
            return -EFAULT;
        memcpy(&next, in, sizeof(next));
        if (next.handler > 1 &&
            (!independent_processes || !signal_supported(signo) || (next.flags & SA_ONSTACK)))
            return -ENOSYS;
    }
    if (old) {
        void *out = guest_ptr(old, sizeof(next), PROT_WRITE);
        if (!out)
            return -EFAULT;
        memcpy(out, &dispositions[signo], sizeof(next));
    }
    if (action) {
        if (next.handler > 1 && !guest_process->sigreturn_pc)
            signal_trampoline();
        dispositions[signo] = next;
        if (next.handler == 1)
            guest_process->pending_signals &= ~(1ULL << (signo - 1));
        if (signo == SIGCHLD && next.handler <= 1)
            guest_process->child_signal_pid = 0;
    }
    return 0;
}
struct linux_sigaction *process_action(struct guest_process *p, int sig) {
    return &p->descriptors->actions[sig];
}
struct linux_sigaction *child_action(struct guest_process *p) {
    return &p->descriptors->actions[SIGCHLD];
}
int socket_family(u64 fd) { return fd < GUEST_FDS ? families[fd] : 0; }
int export_native_fd(unsigned fd) {
    if (fd < GUEST_FDS && shm_files[fd])
        return (shm_flags(shm_files[fd]) & O_ACCMODE) == O_RDWR ? shm_raw(shm_files[fd]) : -1;
    return host_descriptor(fd);
}
int host_descriptor(u64 fd) {
    if (fd < GUEST_FDS && (device[fd] || event_fd(fd) || fd_table()->proc[fd]))
        return -1;
    if (fd < GUEST_FDS && state_fds[fd])
        return state_fds[fd] - 1;
    if (fd < GUEST_FDS && files[fd])
        return files[fd]->fd;
    return fd < 3 && !closed_stdio[fd] ? (int)fd : -1;
}
static const char *program, *exec_path;
static size_t directory_length;
void allow_program_path(const char *path, const char *executable) {
    if (*executable == '/')
        exec_path = executable;
    if (*path != '/' || strstr(path, "/.") || strstr(path, "//"))
        return;
    program = path;
    directory_length = strrchr(program, '/') - program + 1;
}
static int library_path(const char *s) {
    const char *prefix = "/lib/aarch64-linux-gnu/";
    if (!strncmp(s, prefix, strlen(prefix))) {
        const char *name = s + strlen(prefix);
        return *name && *name != '.' && !strchr(name, '/');
    }
    if (!program || strncmp(s, program, directory_length))
        return 0;
    const char *name = s + directory_length;
    const char *suffix = strstr(name, ".so");
    if (!strcmp(program + directory_length, "chromium-shell") ||
        !strcmp(program + directory_length, "content_shell")) {
        static const char *const resources[] = {"icudtl.dat",          "v8_context_snapshot.bin",
                                                "snapshot_blob.bin",   "content_shell.pak",
                                                "shell_resources.pak", "locales/en-US.pak"};
        for (size_t i = 0; i < sizeof(resources) / sizeof(resources[0]); ++i)
            if (!strcmp(name, resources[i]))
                return 1;
    }
    return !strcmp(s, program) ||
           (*name != '.' && !strchr(name, '/') && suffix && (!suffix[3] || suffix[3] == '.'));
}
// After a failed open, ld.so probes the directory before caching its existence.
static int search_directory(const char *s) {
    size_t n = strlen(s);
    if (n && s[n - 1] == '/')
        --n;
    const char *libdir = "/lib/aarch64-linux-gnu";
    return (n == strlen(libdir) && !strncmp(s, libdir, n)) ||
           (program && n + 1 == directory_length && !strncmp(s, program, n));
}
static const char *pathname(u64 addr) {
    for (unsigned i = 0; i < 512; i++) {
        char *b = guest_ptr(addr + i, 1, PROT_READ);
        if (!b)
            return NULL;
        if (!*b)
            return guest_ptr(addr, i + 1, PROT_READ);
    }
    return NULL;
}
static long rename_operand(u64 fd, u64 addr, char out[513]) {
    const char *name = pathname(addr);
    if (!name)
        return -EFAULT;
    const char *directory = NULL;
    if (*name != '/' && (int)fd != AT_FDCWD) {
        if (fd >= GUEST_FDS || !state_fds[fd])
            return -EBADF;
        if (!private_paths[fd][0])
            return -EACCES;
        struct stat st;
        if (fstat(state_fds[fd] - 1, &st))
            return -errno;
        if (!S_ISDIR(st.st_mode))
            return -ENOTDIR;
        directory = private_paths[fd];
    }
    return state_entry(name, directory, out);
}
static long open_lib(u64 dirfd, u64 addr, u64 flags, u64 mode) {
    const char *s = pathname(addr);
    if (!s)
        return -EFAULT;
    if (tracing)
        fprintf(stderr, "linux-run: open %s\n", s);
    struct proc_handle *dir = dirfd < GUEST_FDS ? fd_table()->proc[dirfd] : NULL;
    struct proc_task task;
    int node = proc_node(s, dir, &task);
    if (node) {
        int fd = free_guest_fd();
        if (fd < 0)
            return -EMFILE;
        int rc = proc_open(node, task, flags, &fd_table()->proc[fd]);
        if (!rc)
            fd_table()->cloexec[fd] = !!(flags & O_CLOEXEC);
        return rc ? rc : fd;
    }
    if (fs_confined())
        return -ENOENT;
    if (shm_path(s) && independent_processes) {
        int fd = free_guest_fd();
        if (fd < 0)
            return fd;
        int rc = shm_open_file(s, flags, mode, &shm_files[fd]);
        if (!rc)
            fd_table()->cloexec[fd] = !!(flags & O_CLOEXEC);
        return rc ? rc : fd;
    }
    int random = !strcmp(s, "/dev/urandom");
    int null = !strcmp(s, "/dev/null");
    if (null && (flags & O_DIRECTORY))
        return -ENOTDIR;
    if (null && ((flags & O_ACCMODE) == 3 || (flags & ~(u64)(O_ACCMODE | O_CLOEXEC | O_LARGEFILE |
                                                             O_NOFOLLOW | O_NONBLOCK | O_TRUNC))))
        return -EINVAL;
    char mapped[513];
    int private_file = state_path(s, mapped);
    int resource = state_resource(s, mapped);
    if (!private_file && !resource && !library_path(s) && !random && !null)
        return -ENOENT;
    if (!private_file && !null &&
        (flags & ~(u64)(O_CLOEXEC | O_LARGEFILE | O_NOFOLLOW | O_DIRECTORY | O_NONBLOCK)))
        return -EACCES;
    int i = free_guest_fd();
    if (i < 0)
        return i;
    fd_table()->cloexec[i] = !!(flags & O_CLOEXEC);
    if (private_file || resource) {
        int fd = open(mapped, flags, mode & 0777);
        if (fd < 0)
            return -errno;
        if (private_file)
            strcpy(private_paths[i], mapped);
        state_fds[i] = fd + 1;
        fd_table()->cloexec[i] = fcntl(fd, F_GETFD) == FD_CLOEXEC;
        return i;
    }
    if (random || null) {
        device[i] = random ? RANDOM_DEVICE : NULL_DEVICE + (flags & O_ACCMODE);
        return i;
    }
    files[i] = open_backing(s);
    if (!files[i])
        return -errno;
    if (tracing)
        fprintf(stderr, "linux-run: opened %s (%llu bytes)\n", s,
                (unsigned long long)files[i]->size);
    return i;
}
static long status(u64 guest_fd, u64 dest) {
    int fd = host_descriptor(guest_fd);
    struct stat st = {0};
    _Static_assert(sizeof st == 128, "AArch64 Linux stat ABI");
    void *p = guest_ptr(dest, sizeof st, PROT_WRITE);
    if (!p)
        return -EFAULT;
    if (guest_fd < GUEST_FDS && shm_files[guest_fd]) {
        long rc = shm_stat_handle(shm_files[guest_fd], &st);
        if (rc)
            return rc;
    } else if (guest_fd < GUEST_FDS && fd_table()->proc[guest_fd]) {
        proc_fstat(fd_table()->proc[guest_fd], &st);
    } else if (guest_fd < GUEST_FDS && event_fd(guest_fd)) {
        st.st_mode = channel_stat_mode(guest_fd);
        st.st_nlink = 1;
    } else if (guest_fd < GUEST_FDS && device[guest_fd]) {
        st.st_mode = S_IFCHR | (device[guest_fd] == RANDOM_DEVICE ? 0444 : 0666);
        st.st_nlink = 1;
    } else if (fd < 0 || fstat(fd, &st) < 0) {
        return fd < 0 ? -EBADF : -errno;
    } else if (guest_fd < GUEST_FDS && private_paths[guest_fd][0]) {
        st.st_uid = st.st_gid = 1000;
    }
    memcpy(p, &st, sizeof st);
    return 0;
}
// The native file ABI transfers at most one page per call. A guest buffer of
// this size can straddle two independently backed virtual pages.
static long file_io(int fd, u64 addr, u64 length, int writing, const u64 *offset) {
    size_t size = length < PAGE ? (size_t)length : PAGE;
    int access = writing ? PROT_READ : PROT_WRITE;
    void *data = size ? guest_ptr(addr, size, access) : NULL;
    unsigned char buffer[PAGE];
    size_t first_size = PAGE - addr % PAGE;
    void *first = NULL, *second = NULL;
    if (size && !data) {
        if (size <= first_size)
            return -EFAULT;
        first = guest_ptr(addr, first_size, access);
        second = guest_ptr(addr + first_size, size - first_size, access);
        if (!first || !second)
            return -EFAULT;
        data = buffer;
        if (writing) {
            memcpy(buffer, first, first_size);
            memcpy(buffer + first_size, second, size - first_size);
        }
    }
    ssize_t n;
    if (offset)
        n = writing ? pwrite(fd, data, size, *offset) : pread(fd, data, size, *offset);
    else
        n = writing ? write(fd, data, size) : read(fd, data, size);
    if (n < 0)
        return -errno;
    if (first && !writing && n) {
        size_t copied = (size_t)n < first_size ? (size_t)n : first_size;
        memcpy(first, buffer, copied);
        if ((size_t)n > first_size)
            memcpy(second, buffer + first_size, (size_t)n - first_size);
    }
    return n;
}
static long io(u64 fd, u64 addr, u64 length, int writing) {
    if (fd < GUEST_FDS && shm_files[fd])
        return shm_io(shm_files[fd], addr, length, writing, NULL);
    if (fd < GUEST_FDS && fd_table()->proc[fd])
        return proc_io(fd_table()->proc[fd], addr, length, writing);
    if (fd < GUEST_FDS && event_fd(fd))
        return event_io(thread_cpu(), fd, addr, length, writing);
    if (fd < GUEST_FDS && device[fd] >= NULL_DEVICE) {
        int mode = device[fd] - NULL_DEVICE;
        if ((writing && mode == O_RDONLY) || (!writing && mode == O_WRONLY))
            return -EBADF;
        return writing ? (long)length : 0;
    }
    if (!writing && fd < GUEST_FDS && device[fd] == RANDOM_DEVICE) {
        void *p = guest_ptr(addr, length, PROT_WRITE);
        if (!p)
            return -EFAULT;
        ssize_t n = getrandom(p, length, 0);
        return n < 0 ? -errno : n;
    }
    int actual = host_descriptor(fd);
    if ((writing && fd != 1 && fd != 2 && !(fd < GUEST_FDS && state_fds[fd])) ||
        (!writing && fd < 3 && !state_fds[fd] && !files[fd]) || actual < 0)
        return -EBADF;
    if (socket_family(fd)) {
        CPU *c = thread_cpu();
        for (;;) {
            long n = socket_chunk(actual, addr, length, writing, 0);
            if (n != -EAGAIN || (fcntl(actual, F_GETFL) & O_NONBLOCK))
                return n;
            thread_pause();
            usleep(1000);
            thread_resume(c);
            if (host_descriptor(fd) != actual || !socket_family(fd))
                return -EBADF;
        }
    }
    return file_io(actual, addr, length, writing, NULL);
}
int free_guest_fd(void) {
    for (unsigned fd = 0; fd < guest_process->nofile_soft; ++fd)
        if (!fd_occupied(fd))
            return (int)fd;
    if (tracing)
        fprintf(stderr, "linux-run: pid %d exhausted %u guest descriptors\n", process_pid(),
                guest_process->nofile_soft);
    return -EMFILE;
}
long adopt_fd(int fd) {
    if (fd < 0)
        return -errno;
    int slot = free_guest_fd();
    if (slot < 0) {
        close(fd);
        return slot;
    }
    state_fds[slot] = fd + 1;
    fd_table()->cloexec[slot] = fcntl(fd, F_GETFD) == FD_CLOEXEC;
    return slot;
}
static long exclusive_lock(CPU *, unsigned, int, int, int);
int fd_occupied(unsigned fd) {
    return shm_files[fd] || event_fd(fd) || fd_table()->proc[fd] || files[fd] || device[fd] ||
           state_fds[fd] || (fd < 3 && !closed_stdio[fd]);
}
// A forked Linux fd table can name the same native description in several
// processes. Only the last guest reference closes the enclosing LixinOS fd.
static unsigned extra_refs[GUEST_FDS];
static int retain_raw(int raw) {
    if (raw < 0 || raw >= GUEST_FDS)
        return -EBADF;
    ++extra_refs[raw];
    return raw;
}
static void release_raw(int raw) {
    if (extra_refs[raw])
        --extra_refs[raw];
    else
        close(raw);
}
static void release_ranges(struct guest_files *table, unsigned fd, int owner) {
    struct lx_record_request req = {0, LX_LOCK_LAST, owner, LX_LOCK_UNLOCK};
    for (unsigned i = 0; i < GUEST_FDS; i++) {
        if (table->ranges[i] && !strcmp(table->paths[i], table->paths[fd])) {
            lx_record_lock(table->native[i] - 1, 0, &req);
            table->ranges[i] = 0;
        }
    }
}
static long close_guest(CPU *cpu, u64 fd) {
    if (fd >= GUEST_FDS || !fd_occupied(fd))
        return -EBADF;
    event_closed(fd);
    shm_drop(shm_files[fd]);
    shm_files[fd] = NULL;
    if (fd_table()->proc[fd]) {
        proc_close(fd_table()->proc[fd]);
        fd_table()->proc[fd] = NULL;
    }
    if (state_fds[fd]) {
        if (private_paths[fd][0]) {
            release_ranges(fd_table(), fd, guest_process->pid);
            exclusive_lock(cpu, fd, 0, 1, 0);
            exclusive_lock(cpu, fd, 1, 1, 0);
            private_paths[fd][0] = 0;
        }
        release_raw(state_fds[fd] - 1);
        state_fds[fd] = 0;
    }
    if (files[fd]) {
        close_backing(files[fd]);
        files[fd] = NULL;
    }
    if (fd < 3)
        closed_stdio[fd] = 1;
    device[fd] = families[fd] = fd_table()->cloexec[fd] = 0;
    return 0;
}
void files_on_exec(CPU *cpu) {
    for (int signo = 1; signo < 65; ++signo)
        if (dispositions[signo].handler > 1)
            dispositions[signo] = (struct linux_sigaction){0};
    for (unsigned fd = 0; fd < GUEST_FDS; fd++) {
        if (!fd_occupied(fd))
            continue;
        int flags;
        if (event_fd(fd))
            flags = event_fcntl(fd, F_GETFD, 0);
        else
            flags = fd_table()->cloexec[fd] ? FD_CLOEXEC : 0;
        if (flags < 0 || (flags & FD_CLOEXEC))
            close_guest(cpu, fd);
    }
}
static long duplicate_guest(CPU *cpu, u64 src, u64 target, int cloexec, int exact) {
    if (src >= GUEST_FDS || !fd_occupied(src))
        return -EBADF;
    unsigned limit = guest_process->nofile_soft;
    if (target >= limit)
        return exact ? -EBADF : -EINVAL;
    if (exact && src == target)
        return -EINVAL;
    while (!exact && target < limit && fd_occupied(target))
        ++target;
    if (target == limit)
        return -EMFILE;
    int copy = -1;
    if (state_fds[src] || (src < 3 && !closed_stdio[src])) {
        copy = retain_raw(host_descriptor(src));
        if (copy < 0)
            return copy;
    }
    if (fd_occupied(target))
        close_guest(cpu, target);
    if (copy >= 0) {
        state_fds[target] = copy + 1;
        families[target] = families[src];
        strcpy(private_paths[target], private_paths[src]);
    } else if (shm_files[src]) {
        shm_files[target] = shm_files[src];
        shm_retain(shm_files[src]);
    } else if (files[src]) {
        files[target] = files[src];
        ++files[src]->refs;
        ++files[src]->open;
    } else if (event_fd(src)) {
        channel_duplicate(src, target, cloexec);
    } else if (fd_table()->proc[src]) {
        fd_table()->proc[target] = fd_table()->proc[src];
        proc_inherit(fd_table()->proc[target]);
    } else {
        device[target] = device[src];
    }
    event_set_key(target, event_key(src));
    fd_table()->cloexec[target] = cloexec;
    return target;
}
// The queued message owns a reference, independently of either process's fd
// numbers. In particular a read-only shm view keeps its original access rights.
struct held_fd {
    u64 key;
    struct held_fd *next;
    struct held_channel *channel;
    struct shm_handle *shm;
    struct backing *file;
    struct proc_handle *proc;
    int raw, special, family;
};
static struct held_fd *in_transit;
int held_event_key(u64 key) {
    for (struct held_fd *h = in_transit; h; h = h->next)
        if (h->key == key)
            return 1;
    return 0;
}
void drop_fd(struct held_fd *h) {
    if (!h)
        return;
    if (h->key) {
        struct held_fd **link = &in_transit;
        while (*link != h)
            link = &(*link)->next;
        *link = h->next;
        forget_event_key(h->key);
    }
    if (h->raw >= 0)
        release_raw(h->raw);
    if (h->file)
        close_backing(h->file);
    proc_close(h->proc);
    shm_drop(h->shm);
    drop_channel(h->channel);
    free(h);
}
long hold_fd(unsigned fd, unsigned transport, struct held_fd **out) {
    if (fd >= GUEST_FDS || !fd_occupied(fd))
        return -EBADF;
    struct held_fd *h = calloc(1, sizeof *h);
    if (!h)
        return -ENOMEM;
    h->raw = -1;
    int rc = 0;
    if (event_fd(fd)) {
        rc = hold_channel(fd, transport, &h->channel);
    } else if (shm_files[fd]) {
        shm_retain(h->shm = shm_files[fd]);
    } else if (files[fd]) {
        h->file = files[fd];
        ++h->file->refs;
        ++h->file->open;
    } else if (fd_table()->proc[fd]) {
        proc_inherit(h->proc = fd_table()->proc[fd]);
    } else if (device[fd]) {
        h->special = device[fd];
    } else {
        h->raw = retain_raw(host_descriptor(fd));
        if (h->raw < 0)
            rc = h->raw;
        h->family = families[fd];
    }
    if (rc) {
        drop_fd(h);
        return rc;
    }
    h->key = event_key(fd);
    h->next = in_transit;
    in_transit = h;
    *out = h;
    return 0;
}
long receive_fd(const struct held_fd *h, int cloexec) {
    int fd = free_guest_fd();
    if (fd < 0)
        return fd;
    if (h->channel) {
        receive_channel(h->channel, fd, cloexec);
    } else if (h->shm) {
        shm_retain(shm_files[fd] = h->shm);
    } else if (h->file) {
        files[fd] = h->file;
        ++h->file->refs;
        ++h->file->open;
    } else if (h->proc) {
        proc_inherit(fd_table()->proc[fd] = h->proc);
    } else if (h->special) {
        device[fd] = h->special;
    } else {
        int copy = retain_raw(h->raw);
        if (copy < 0)
            return copy;
        state_fds[fd] = copy + 1;
        families[fd] = h->family;
    }
    fd_table()->cloexec[fd] = cloexec;
    event_set_key(fd, h->key);
    return fd;
}
static long unix_address(u64 addr, u64 len, struct sockaddr_un *out) {
    if (len < sizeof(sa_family_t) || len > sizeof(*out))
        return -EINVAL;
    const struct sockaddr_un *in = guest_ptr(addr, len, PROT_READ);
    if (!in)
        return -EFAULT;
    if (in->sun_family != AF_UNIX)
        return -EAFNOSUPPORT;
    size_t n = len - offsetof(struct sockaddr_un, sun_path);
    if (!n)
        return -EINVAL;
    *out = (struct sockaddr_un){.sun_family = AF_UNIX};
    char path[513];
    if (!in->sun_path[0]) {
        // Scope Linux abstract socket names to this application's private directory.
        if (!state_path("/run/user/1000", path))
            return -EACCES;
        size_t k = strlen(path);
        if (guest_process->net_namespace) {
            int added = snprintf(path + k, sizeof(path) - k, "/net-%d-%d", getpid(),
                                 guest_process->net_namespace);
            if (added < 0 || (size_t)added >= sizeof(path) - k)
                return -ENAMETOOLONG;
            k += added;
        }
        if (k + n + 1 > sizeof out->sun_path)
            return -ENAMETOOLONG;
        memcpy(out->sun_path + 1, path, k + 1);
        memcpy(out->sun_path + k + 2, in->sun_path + 1, n - 1);
        return offsetof(struct sockaddr_un, sun_path) + k + n + 1;
    }
    if (fs_confined())
        return -ENOENT;
    char name[sizeof in->sun_path + 1];
    memcpy(name, in->sun_path, n);
    name[n] = 0;
    if (!state_path(name, path))
        return -EACCES;
    if (strlen(path) >= sizeof out->sun_path)
        return -ENAMETOOLONG;
    strcpy(out->sun_path, path);
    return offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1;
}
// A live abstract socket reserves a name atomically across LixinOS
// processes. The kernel drops it on last close, including abnormal exit.
static long exclusive_lock(CPU *c, unsigned fd, int posix, int unlock, int blocking) {
    if (fd >= GUEST_FDS || !private_paths[fd][0])
        return -EBADF;
    int *locks = posix ? record_locks : flock_locks;
    for (int i = 3; i < GUEST_FDS; i++) {
        if ((i == (int)fd || (posix && !strcmp(private_paths[fd], private_paths[i]))) && locks[i]) {
            if (unlock) {
                close(locks[i] - 1);
                locks[i] = 0;
            }
            return 0;
        }
    }
    if (unlock)
        return 0;
    // The runtime's file identity is its canonical VFS path.
    struct sockaddr_un key = {.sun_family = AF_UNIX};
    size_t length = strlen(private_paths[fd]);
    if (length + 4 > sizeof key.sun_path)
        return -ENAMETOOLONG;
    key.sun_path[1] = 'L';
    key.sun_path[2] = posix ? 'P' : 'F';
    memcpy(key.sun_path + 3, private_paths[fd], length);
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0)
        return -errno;
    for (;;) {
        if (!bind(sock, (void *)&key, offsetof(struct sockaddr_un, sun_path) + 3 + length)) {
            locks[fd] = sock + 1;
            return 0;
        }
        int e = errno;
        if (e != EADDRINUSE || !blocking) {
            close(sock);
            return e == EADDRINUSE ? -EAGAIN : -e;
        }
        thread_pause();
        usleep(10000);
        thread_resume(c);
        if (fd >= GUEST_FDS || !private_paths[fd][0] ||
            strcmp(private_paths[fd], key.sun_path + 3)) {
            close(sock);
            return -EBADF;
        }
    }
}
static long record_lock(CPU *cpu, unsigned fd, int cmd, u64 address) {
    struct flock lock;
    if (!guest_copy(address, &lock, sizeof lock, 0)) return -EFAULT;
    if (!private_paths[fd][0]) return -EBADF;
    if (lock.l_type != F_RDLCK && lock.l_type != F_WRLCK && lock.l_type != F_UNLCK)
        return -EINVAL;
    int raw = host_descriptor(fd);
    int flags = fcntl(raw, F_GETFL);
    if (flags < 0) return -errno;
    if (cmd != F_GETLK && ((lock.l_type == F_RDLCK && (flags & O_ACCMODE) == O_WRONLY) ||
                           (lock.l_type == F_WRLCK && (flags & O_ACCMODE) == O_RDONLY)))
        return -EBADF;
    off_t base = 0, first, last = LX_LOCK_LAST;
    if (lock.l_whence == SEEK_CUR) {
        base = lseek(raw, 0, SEEK_CUR);
        if (base < 0) return -errno;
    } else if (lock.l_whence == SEEK_END) {
        struct stat st;
        if (fstat(raw, &st)) return -errno;
        base = st.st_size;
    } else if (lock.l_whence != SEEK_SET) return -EINVAL;
    if (__builtin_add_overflow(base, lock.l_start, &first)) return -EOVERFLOW;
    if (first < 0) return -EINVAL;
    if (lock.l_len < 0) {
        last = first - 1;
        if (lock.l_len < -first) return -EINVAL;
        first += lock.l_len;
    } else if (lock.l_len && __builtin_add_overflow(first, lock.l_len - 1, &last)) {
        return -EOVERFLOW;
    }
    struct lx_record_request req = {first, last, guest_process->pid, lock.l_type};
    u64 key = event_key(fd);
    for (;;) {
        long r = lx_record_lock(raw, cmd == F_GETLK, &req);
        if (r == -ENOSYS) {
            /* Older released kernels support whole-file exclusive locks through
             * the abstract socket namespace. */
            if (cmd == F_GETLK || first || last != LX_LOCK_LAST || lock.l_type == F_RDLCK)
                return -ENOTSUP;
            return exclusive_lock(cpu, fd, 1, lock.l_type == F_UNLCK, cmd == F_SETLKW);
        }
        if (r != -EAGAIN || cmd != F_SETLKW) {
            if (r) return r;
            if (cmd == F_GETLK) {
                lock.l_type = req.type;
                if (req.type != LX_LOCK_UNLOCK) {
                    lock.l_pid = req.guest;
                    lock.l_whence = SEEK_SET;
                    lock.l_start = req.first;
                    lock.l_len = req.last == LX_LOCK_LAST ? 0 : req.last - req.first + 1;
                }
                return guest_copy(address, &lock, sizeof lock, 1) ? 0 : -EFAULT;
            }
            if (lock.l_type != F_UNLCK) fd_table()->ranges[fd] = 1;
            return 0;
        }
        thread_pause();
        usleep(10000);
        thread_resume(cpu);
        if (signal_ready()) return -EINTR;
        if (event_key(fd) != key || host_descriptor(fd) != raw) return -EBADF;
    }
}
long service(CPU *c) {
    const u64 *a = c->x;
    if (tracing)
        fprintf(stderr, "linux-run: pid=%d tid=%d syscall %llu\n", guest_process->pid, thread_tid(), (unsigned long long)a[8]);
    if (a[8] == SYS_openat || a[8] == SYS_pipe2 || a[8] == SYS_socket || a[8] == SYS_socketpair ||
        a[8] == SYS_flock || a[8] == SYS_fcntl || a[8] == SYS_memfd_create) {
        unsigned used = 2;
        for (int i = 3; i < GUEST_FDS; i++)
            used += (state_fds[i] != 0) + (record_locks[i] != 0) + (flock_locks[i] != 0);
        reserve_descriptors(used);
    }
    switch (a[8]) {
    case SYS_eventfd2:
    case SYS_epoll_create1:
    case SYS_epoll_ctl:
        return event_service(c);
    case SYS_epoll_pwait:
    case SYS_ppoll:
        return masked_poll(c);
    case SYS_rt_sigreturn:
        return signal_return(c);
    case LX_RESOLVE_IPV4: {
        if (guest_process->net_namespace)
            return -ENETUNREACH;
        const char *name = pathname(a[0]);
        unsigned char *ip = guest_ptr(a[1], 4, PROT_WRITE);
        if (!name || !ip)
            return -EFAULT;
        int r = lx_lookup_ipv4(name, ip);
        if (getenv("LIXINOS_NET_TRACE"))
            fprintf(stderr, "linux-run: DNS %s %s\n", name, r < 0 ? "failed" : "resolved");
        return r < 0 ? -ENOENT : 0;
    }
    case SYS_uname: {
        struct utsname *u = guest_ptr(a[0], sizeof(*u), PROT_WRITE);
        if (!u)
            return -EFAULT;
        return uname(u) < 0 ? -errno : 0;
    }
    case SYS_sendmsg:
    case SYS_recvmsg:
    case SYS_sendto:
    case SYS_recvfrom:
        return message_service(c);
    case SYS_shutdown: {
        if (a[0] < GUEST_FDS && event_fd(a[0]))
            return channel_shutdown(a[0], a[1]);
        int fd = host_descriptor(a[0]);
        return fd < 0 ? -EBADF : shutdown(fd, a[1]) < 0 ? -errno : 0;
    }
    case SYS_memfd_create: {
        const char *name = pathname(a[0]);
        if (!name)
            return -EFAULT;
        return adopt_fd(syscall(SYS_memfd_create, name, a[1]));
    }
    case SYS_socket:
        if (tracing)
            fprintf(stderr, "linux-run: socket family=%lu type=%#lx proto=%lu\n",
                    (unsigned long)a[0], (unsigned long)a[1], (unsigned long)a[2]);
        // A new guest network namespace starts without an IPv4 interface.
        if (a[0] == AF_INET && guest_process->net_namespace)
            return -EAFNOSUPPORT;
        if (a[0] != AF_UNIX && a[0] != AF_INET)
            return -EAFNOSUPPORT;
        {
            long fd = adopt_fd(socket(a[0], a[1], a[2]));
            if (fd >= 0)
                families[fd] = a[0];
            return fd;
        }
    case SYS_socketpair:
    case SYS_pipe2:
        return channel_pair(c);
    case SYS_bind:
    case SYS_connect: {
        const sa_family_t *family = guest_ptr(a[1], sizeof *family, PROT_READ);
        if (a[2] < sizeof *family || !family)
            return -EINVAL;
        if (*family == AF_INET) {
            if (a[2] < sizeof(struct sockaddr_in))
                return -EINVAL;
            struct sockaddr_in *peer = guest_ptr(a[1], sizeof *peer, PROT_READ);
            int fd = host_descriptor(a[0]);
            if (!peer || fd < 0)
                return !peer ? -EFAULT : -EBADF;
            int r = a[8] == SYS_bind ? bind(fd, (void *)peer, sizeof *peer)
                                     : connect(fd, (void *)peer, sizeof *peer);
            int e = errno;
            if (getenv("LIXINOS_NET_TRACE"))
                fprintf(stderr, "linux-run: IPv4 %s port %u result %d\n",
                        a[8] == SYS_bind ? "bind" : "connect", ntohs(peer->sin_port),
                        r < 0 ? -e : 0);
            return r < 0 ? -e : r;
        }
        struct sockaddr_un peer;
        long n = unix_address(a[1], a[2], &peer);
        if (n < 0)
            return n;
        int fd = host_descriptor(a[0]);
        if (fd < 0)
            return -EBADF;
        if (tracing)
            fprintf(stderr, "linux-run: %s %s\n", a[8] == SYS_bind ? "bind" : "connect",
                    peer.sun_path[0] ? peer.sun_path : "(abstract)");
        int r = a[8] == SYS_bind ? bind(fd, (void *)&peer, n) : connect(fd, (void *)&peer, n);
        return r < 0 ? -errno : 0;
    }
    case SYS_getsockopt:
    case SYS_setsockopt: {
        if (a[0] < GUEST_FDS && event_fd(a[0]))
            return channel_sockopt(c);
        int raw = host_descriptor(a[0]);
        if (raw < 0)
            return event_fd((int)a[0]) ? -ENOPROTOOPT : -EBADF;
        if (a[8] == SYS_setsockopt) {
            void *value = guest_ptr(a[3], a[4], PROT_READ);
            if (!value)
                return -EFAULT;
            return setsockopt(raw, a[1], a[2], value, a[4]) < 0 ? -errno : 0;
        }
        socklen_t *len = guest_ptr(a[4], sizeof *len, PROT_READ | PROT_WRITE);
        if (!len)
            return -EFAULT;
        void *value = guest_ptr(a[3], *len, PROT_WRITE);
        if (!value)
            return -EFAULT;
        return getsockopt(raw, a[1], a[2], value, len) < 0 ? -errno : 0;
    }
    case SYS_listen: {
        int fd = host_descriptor(a[0]);
        return fd < 0 ? -EBADF : listen(fd, a[1]) < 0 ? -errno : 0;
    }
    case SYS_flock: {
        int op = a[1] & ~4;
        if (op != 2 && op != 8)
            return -ENOTSUP;
        return exclusive_lock(c, a[0], 0, op == 8, !(a[1] & 4));
    }
    case SYS_dup:
        return duplicate_guest(c, a[0], 0, 0, 0);
    case SYS_dup3:
        if (a[2] & ~(u64)O_CLOEXEC)
            return -EINVAL;
        return duplicate_guest(c, a[0], a[1], !!a[2], 1);
    case SYS_fcntl: {
        if (a[1] == F_DUPFD || a[1] == F_DUPFD_CLOEXEC)
            return duplicate_guest(c, a[0], a[2], a[1] == F_DUPFD_CLOEXEC, 0);
        if (a[0] < GUEST_FDS && event_fd(a[0]))
            return event_fcntl(a[0], a[1], a[2]);
        if (a[0] >= GUEST_FDS || !fd_occupied(a[0]))
            return -EBADF;
        if (a[1] == F_GETFD || a[1] == F_SETFD) {
            if (a[1] == F_GETFD)
                return fd_table()->cloexec[a[0]] ? FD_CLOEXEC : 0;
            fd_table()->cloexec[a[0]] = !!(a[2] & FD_CLOEXEC);
            return 0;
        }
        if (fd_table()->proc[a[0]] && a[1] == F_GETFL)
            return proc_flags(fd_table()->proc[a[0]]);
        if (shm_files[a[0]] && a[1] == F_GETFL)
            return shm_flags(shm_files[a[0]]);
        if (device[a[0]] && a[1] == F_GETFL)
            return device[a[0]] >= NULL_DEVICE ? device[a[0]] - NULL_DEVICE : O_RDONLY;
        int fd = host_descriptor(a[0]);
        if (fd < 0)
            return -EBADF;
        if (tracing)
            fprintf(stderr, "linux-run: fcntl cmd=%lu\n", (unsigned long)a[1]);
        if (a[1] == F_GETLK || a[1] == F_SETLK || a[1] == F_SETLKW)
            return record_lock(c, a[0], a[1], a[2]);
        if (a[1] != F_GETFL && a[1] != F_SETFL && a[1] != F_GETFD && a[1] != F_SETFD)
            return -ENOSYS;
        int r = fcntl(fd, a[1], a[2]);
        return r < 0 ? -errno : r;
    }
    case SYS_prlimit64: {
        if ((a[0] && a[0] != (u64)process_pid()) ||
            (a[2] && a[1] != RLIMIT_DATA && a[1] != RLIMIT_NOFILE))
            return -ENOTSUP;
        struct rlimit limit;
        switch (a[1]) {
        case RLIMIT_NOFILE:
            limit = (struct rlimit){guest_process->nofile_soft, guest_process->nofile_hard};
            break;
        case RLIMIT_STACK:
            limit = (struct rlimit){ARENA - STACK_BASE, ARENA - STACK_BASE};
            break;
        case RLIMIT_DATA:
            limit = (struct rlimit){guest_process->data_soft, guest_process->data_hard};
            break;
        case RLIMIT_AS:
        case RLIMIT_CPU:
        case RLIMIT_NPROC:
            limit = (struct rlimit){RLIM_INFINITY, RLIM_INFINITY};
            break;
        case RLIMIT_CORE:
            limit = (struct rlimit){0, 0};
            break;
        default:
            return -ENOTSUP;
        }
        struct rlimit next;
        if (a[2]) {
            void *in = guest_ptr(a[2], sizeof next, PROT_READ);
            if (!in)
                return -EFAULT;
            memcpy(&next, in, sizeof next);
            if (next.rlim_cur > next.rlim_max)
                return -EINVAL;
            if (next.rlim_max > limit.rlim_max)
                return -EPERM;
        }
        if (a[3]) {
            void *out = guest_ptr(a[3], sizeof limit, PROT_WRITE);
            if (!out)
                return -EFAULT;
            memcpy(out, &limit, sizeof limit);
        }
        if (a[2] && a[1] == RLIMIT_NOFILE) {
            guest_process->nofile_soft = (unsigned)next.rlim_cur;
            guest_process->nofile_hard = (unsigned)next.rlim_max;
        } else if (a[2]) {
            guest_process->data_soft = next.rlim_cur;
            guest_process->data_hard = next.rlim_max;
        }
        return 0;
    }
    case SYS_futex: {
        unsigned cmd = a[1] & ~128u; // FUTEX_PRIVATE_FLAG
        if (tracing)
            fprintf(stderr, "linux-run: futex op=%#lx addr=%#lx value=%u\n", (unsigned long)a[1],
                    (unsigned long)a[0], (unsigned)a[2]);
        if (a[0] % sizeof(int))
            return -EINVAL;
        int *word = guest_ptr(a[0], sizeof(*word), PROT_READ);
        if (!word)
            return -EFAULT;
        // FUTEX_WAKE_OP atomically updates the second word before deciding
        // which wait queues to wake. The interpreter holds the guest CPU lock.
        if (cmd == 5) {
            if (a[4] % sizeof(int) || (int)a[2] < 0 || (int)a[3] < 0)
                return -EINVAL;
            unsigned *second = guest_ptr(a[4], 4, PROT_READ | PROT_WRITE);
            if (!second)
                return -EFAULT;
            unsigned encoded = a[5], op = encoded >> 28, cmp = (encoded >> 24) & 15;
            int arg = (int)(encoded << 8) >> 20, ref = (int)(encoded << 20) >> 20;
            if (op & 8) {
                if ((unsigned)arg >= 32)
                    return -EINVAL;
                arg = 1u << arg;
                op &= 7;
            }
            if (cmp > 5 || op > 4)
                return -ENOSYS;
            unsigned old = *second;
            switch (op) {
            case 0:
                *second = arg;
                break;
            case 1:
                *second = old + (unsigned)arg;
                break;
            case 2:
                *second = old | (unsigned)arg;
                break;
            case 3:
                *second = old & ~(unsigned)arg;
                break;
            case 4:
                *second = old ^ (unsigned)arg;
                break;
            }
            int v = (int)old;
            int condition[] = {v == ref, v != ref, v<ref, v <= ref, v> ref, v >= ref};
            long first = thread_futex_wake(word, (int)a[2]);
            if (first < 0 || !condition[cmp])
                return first;
            return first + thread_futex_wake((int *)second, (int)a[3]);
        }
        const struct timespec *timeout = NULL;
        if ((cmd == 0 || (cmd & ~256u) == 9) && a[3]) {
            timeout = guest_ptr(a[3], sizeof(*timeout), PROT_READ);
            if (!timeout)
                return -EFAULT;
        }
        if (cmd != 0 && cmd != 1 && (cmd & ~256u) != 9)
            return -ENOSYS;
        if ((cmd & ~256u) == 9 && (unsigned)a[5] != ~0u)
            return a[5] ? -ENOSYS : -EINVAL;
        struct timespec deadline;
        if (timeout) {
            memcpy(&deadline, timeout, sizeof deadline);
            timeout = &deadline;
        }
        if (cmd == 1)
            return thread_futex_wake(word, (int)a[2]);
        return thread_futex_wait(c, word, (int)a[2], (int)a[1], timeout);
    }
    case SYS_clone:
        return clone_thread(c);
    case SYS_rt_sigaction:
        return linux_sigaction(a[0], a[1], a[2], a[3]);
    case SYS_rt_sigprocmask:
        return thread_mask(a[0], a[1], a[2], a[3]);
    case SYS_unshare:
        return independent_processes ? namespace_unshare(a[0]) : -EINVAL;
    case SYS_prctl:
    case SYS_capset:
    case SYS_capget:
        return thread_caps(c);
    case SYS_getpid:
        return process_pid();
    case SYS_getppid:
        return process_ppid();
    case SYS_kill:
        if (tracing) fprintf(stderr, "linux-run: pid=%d kill(%d, %d)\n", guest_process->pid, (int)a[0], (int)a[1]);
        return signal_process((int)a[0], (int)a[1]);
    case SYS_tgkill:
        if ((int)a[0] != process_pid() || (int)a[1] != thread_tid())
            return -ENOTSUP;
        if (guest_process->threads != 1)
            return -ENOTSUP;
        return signal_process(process_pid(), (int)a[2]);
    case SYS_waitid:
    case SYS_wait4:
        return process_wait(c);
    case SYS_sched_getscheduler:
    case SYS_sched_getparam:
        return thread_sched_query((int)a[0], a[8] == SYS_sched_getscheduler, a[1]);
    case SYS_sched_yield:
        thread_slice(c);
        return 0;
    case SYS_gettid:
        return thread_tid();
    case SYS_getuid:
    case SYS_getgid:
    case SYS_geteuid:
    case SYS_getegid:
        return namespace_id(a[8] == SYS_getgid || a[8] == SYS_getegid);
    case SYS_getresuid:
    case SYS_getresgid: {
        // This ABI currently exposes one unprivileged identity for all three IDs.
        uint32_t id = namespace_id(a[8] == SYS_getresgid);
        for (unsigned i = 0; i < 3; ++i) {
            void *out = guest_ptr(a[i], sizeof(id), PROT_WRITE);
            if (!out)
                return -EFAULT;
            memcpy(out, &id, sizeof(id));
        }
        return 0;
    }
    case SYS_clock_gettime: {
        struct timespec *ts = guest_ptr(a[1], sizeof(*ts), PROT_WRITE);
        if (!ts)
            return -EFAULT;
        if (a[0] == CLOCK_THREAD_CPUTIME_ID || a[0] == CLOCK_PROCESS_CPUTIME_ID) {
            thread_cputime(a[0] == CLOCK_PROCESS_CPUTIME_ID, ts);
            return 0;
        }
        int clock = a[0] == CLOCK_MONOTONIC_COARSE  ? CLOCK_MONOTONIC
                    : a[0] == CLOCK_REALTIME_COARSE ? CLOCK_REALTIME
                                                    : (int)a[0];
        return clock_gettime(clock, ts) < 0 ? -errno : 0;
    }
    case SYS_gettimeofday: {
        struct timeval *tv = a[0] ? guest_ptr(a[0], sizeof(*tv), PROT_WRITE) : NULL;
        int *zone = a[1] ? guest_ptr(a[1], 2 * sizeof(int), PROT_WRITE) : NULL;
        if ((a[0] && !tv) || (a[1] && !zone))
            return -EFAULT;
        if (zone)
            memset(zone, 0, 2 * sizeof(int));
        struct timespec now;
        if (tv) {
            if (clock_gettime(CLOCK_REALTIME, &now))
                return -errno;
            tv->tv_sec = now.tv_sec;
            tv->tv_usec = now.tv_nsec / 1000;
        }
        return 0;
    }
    case SYS_exit:
        thread_exit(a[0] & 255);
    case SYS_exit_group:
        process_exit(a[0] & 255);
    case SYS_chroot:
    case SYS_chdir: {
        const char *path = pathname(a[0]);
        if (!path)
            return -EFAULT;
        return a[8] == SYS_chroot ? fs_chroot(path) : fs_chdir(path);
    }
    case SYS_readlinkat: {
        const char *s = pathname(a[1]);
        if (!s)
            return -EFAULT;
        if (!a[3])
            return -EINVAL;
        if (fs_confined())
            return -ENOENT;
        if (!strcmp(s, "/proc/self/exe") && exec_path) {
            size_t n = strlen(exec_path);
            if (a[3] < n)
                n = a[3];
            void *dst = guest_ptr(a[2], n, PROT_WRITE);
            if (!dst)
                return -EFAULT;
            memcpy(dst, exec_path, n);
            return n;
        }
        // realpath walks each component with readlinkat. Linux returns EINVAL
        // for existing plain files and directories, including virtual mount points.
        if (state_parent(s))
            return -EINVAL;
        struct stat st;
        if (independent_processes && shm_path(s)) {
            long rc = shm_stat_name(s, &st);
            return rc ? rc : -EINVAL;
        }
        char path[513];
        const char *resolved = s;
        if (state_path(s, path) || state_resource(s, path))
            resolved = path;
        else if (!library_path(s) && !search_directory(s))
            return -ENOENT;
        return stat(resolved, &st) < 0 ? -errno : -EINVAL;
    }
    case SYS_execve: {
        if (fs_confined())
            return -ENOENT;
        if (!independent_processes)
            return -ENOSYS;
        const char *path = pathname(a[0]);
        return path ? exec_guest(c, path, a[1], a[2]) : -EFAULT;
    }
    case SYS_openat:
        return open_lib(a[0], a[1], a[2], a[3]);
    case SYS_renameat:
    case SYS_renameat2: {
        if (fs_confined())
            return -EROFS;
        if (a[8] == SYS_renameat2 && a[4])
            return -EOPNOTSUPP;
        char from[513], to[513];
        long rc = rename_operand(a[0], a[1], from);
        if (!rc)
            rc = rename_operand(a[2], a[3], to);
        if (rc)
            return rc;
        return rename(from, to) < 0 ? -errno : 0;
    }
    case SYS_mkdirat:
    case SYS_unlinkat: {
        if (fs_confined())
            return -EROFS;
        const char *s = pathname(a[1]);
        char path[513];
        if (!s)
            return -EFAULT;
        if (tracing)
            fprintf(stderr, "linux-run: %s %s\n", a[8] == SYS_mkdirat ? "mkdir" : "unlink", s);
        if (state_parent(s) && a[8] == SYS_mkdirat)
            return -EEXIST;
        if (independent_processes && shm_path(s) && a[8] == SYS_unlinkat)
            return a[2] ? -EINVAL : shm_remove(s);
        if (!state_path(s, path))
            return -EACCES;
        int r = a[8] == SYS_mkdirat    ? mkdir(path, a[2] & 0777)
                : a[2] == AT_REMOVEDIR ? rmdir(path)
                : !a[2]                ? unlink(path)
                                       : -1;
        if (a[8] == SYS_unlinkat && a[2] && a[2] != AT_REMOVEDIR)
            return -EINVAL;
        return r < 0 ? -errno : 0;
    }
    case SYS_lseek: {
        if (a[0] < GUEST_FDS && shm_files[a[0]])
            return shm_seek(shm_files[a[0]], a[1], a[2]);
        int fd = host_descriptor(a[0]);
        if (fd < 0)
            return -EBADF;
        off_t r = lseek(fd, a[1], a[2]);
        return r < 0 ? -errno : r;
    }
    case SYS_getdents64: {
        if (a[0] < GUEST_FDS && fd_table()->proc[a[0]])
            return proc_entries(fd_table()->proc[a[0]], a[1], a[2]);
        int fd = host_descriptor(a[0]);
        if (fd < 0)
            return -EBADF;
        void *dest = guest_ptr(a[1], a[2], PROT_WRITE);
        if (!dest)
            return -EFAULT;
        long r = syscall(SYS_getdents64, fd, dest, a[2]);
        return r < 0 ? -errno : r;
    }
    case SYS_pwrite64:
    case SYS_pread64: {
        if (a[0] < GUEST_FDS && shm_files[a[0]]) {
            off_t where = a[3];
            return shm_io(shm_files[a[0]], a[1], a[2], a[8] == SYS_pwrite64, &where);
        }
        int writing = a[8] == SYS_pwrite64;
        if (writing && (a[0] >= GUEST_FDS || !state_fds[a[0]]))
            return -EBADF;
        int fd = host_descriptor(a[0]);
        if (fd < 0)
            return -EBADF;
        return file_io(fd, a[1], a[2], writing, &a[3]);
    }
    case SYS_fsync:
    case SYS_fdatasync: {
        int fd = host_descriptor(a[0]);
        return fd < 0 ? -EBADF : fsync(fd) < 0 ? -errno : 0;
    }
    case SYS_close:
        return close_guest(c, a[0]);
    case SYS_read:
        return io(a[0], a[1], a[2], 0);
    case SYS_write:
        return io(a[0], a[1], a[2], 1);
    case SYS_writev: {
        if (a[2] > 1024)
            return -EINVAL;
        unsigned char *iov = guest_ptr(a[1], a[2] * 16, PROT_READ);
        if (!iov)
            return -EFAULT;
        long total = 0;
        for (size_t i = 0; i < a[2]; i++) {
            u64 item[2];
            memcpy(item, iov + i * 16, sizeof item);
            if (!item[1])
                continue;
            long n = io(a[0], item[0], item[1], 1);
            if (n < 0)
                return total ? total : n;
            total += n;
            if ((u64)n != item[1])
                break;
        }
        return total;
    }
    case SYS_statfs:
    case SYS_fstatfs: {
        struct statfs st;
        int rc;
        if (a[8] == SYS_statfs) {
            const char *path = pathname(a[0]);
            if (!path) return -EFAULT;
            if (fs_confined()) return -ENOENT;
            if (!shm_path(path) || !independent_processes) return -ENOSYS;
            struct stat entry;
            long result = shm_stat_name(path, &entry);
            if (result) return result;
            rc = statfs("/dev/shm", &st);
        } else {
            if (a[0] >= GUEST_FDS || !fd_occupied(a[0])) return -EBADF;
            int fd = shm_files[a[0]] ? shm_raw(shm_files[a[0]]) : host_descriptor(a[0]);
            if (fd < 0) return -ENOSYS;
            rc = fstatfs(fd, &st);
        }
        if (rc) return -errno;
        return guest_copy(a[1], &st, sizeof st, 1) ? 0 : -EFAULT;
    }
    case SYS_fstat:
        return status(a[0], a[1]);
    case SYS_newfstatat: {
        const char *s = pathname(a[1]);
        if (!s)
            return -EFAULT;
        struct proc_task task;
        int node = proc_node(s, a[0] < GUEST_FDS ? fd_table()->proc[a[0]] : NULL, &task);
        if (node) {
            struct stat *out = guest_ptr(a[2], sizeof *out, PROT_WRITE);
            if (!out)
                return -EFAULT;
            struct stat st;
            proc_stat(node, task, &st);
            memcpy(out, &st, sizeof st);
            return 0;
        }
        struct proc_handle *dir = a[0] < GUEST_FDS ? fd_table()->proc[a[0]] : NULL;
        int target = proc_target_fd(dir, s);
        if (target >= 0)
            return fd_occupied(target) ? status(target, a[2]) : -ENOENT;
        if (!*s && (a[3] & AT_EMPTY_PATH))
            return status(a[0], a[2]);
        if (fs_confined())
            return -ENOENT;
        char mapped[513];
        if (independent_processes && shm_path(s)) {
            struct stat *st = guest_ptr(a[2], sizeof *st, PROT_WRITE);
            return st ? shm_stat_name(s, st) : -EFAULT;
        }
        int local = state_path(s, mapped);
        if (state_parent(s)) {
            struct stat *st = guest_ptr(a[2], sizeof(*st), PROT_WRITE);
            if (!st)
                return -EFAULT;
            memset(st, 0, sizeof(*st));
            st->st_mode = S_IFDIR | 0555;
            st->st_nlink = 2;
            return 0;
        }
        int resource = state_resource(s, mapped);
        if (local || resource || library_path(s) || search_directory(s)) {
            struct stat st = {0};
            void *p = guest_ptr(a[2], sizeof st, PROT_WRITE);
            if (!p)
                return -EFAULT;
            if (stat(local || resource ? mapped : s, &st) < 0)
                return -errno;
            if (local)
                st.st_uid = st.st_gid = 1000;
            memcpy(p, &st, sizeof st);
            return 0;
        }
        return -ENOENT;
    }
    case SYS_faccessat: {
        const char *s = pathname(a[1]);
        if (!s)
            return -EFAULT;
        struct proc_task task;
        int node = proc_node(s, a[0] < GUEST_FDS ? fd_table()->proc[a[0]] : NULL, &task);
        if (node) {
            if (a[2] & ~(R_OK | W_OK | X_OK))
                return -EINVAL;
            struct stat st;
            proc_stat(node, task, &st);
            if (((a[2] & W_OK) && !(st.st_mode & 0200)) || ((a[2] & X_OK) && !(st.st_mode & 0100)))
                return -EACCES;
            return 0;
        }
        if (fs_confined())
            return -ENOENT;
        char mapped[513];
        if (independent_processes && shm_path(s)) {
            struct stat st;
            long rc = shm_stat_name(s, &st);
            if (rc)
                return rc;
            return (a[2] & ~(u64)((st.st_mode >> 6) & 7)) ? -EACCES : 0;
        }
        if (state_parent(s))
            return (a[2] & W_OK) ? -EACCES : 0;
        if (state_path(s, mapped))
            return access(mapped, a[2]) < 0 ? -errno : 0;
        if (state_resource(s, mapped))
            return (a[2] & W_OK) ? -EACCES : access(mapped, a[2]) < 0 ? -errno : 0;
        if (!library_path(s))
            return -ENOENT;
        if (a[2] & ~R_OK)
            return -EACCES;
        struct stat st;
        return stat(s, &st) < 0 ? -errno : 0;
    }
    case SYS_fallocate:
    case SYS_ftruncate: {
        if (a[0] < GUEST_FDS && shm_files[a[0]]) {
            struct shm_handle *h = shm_files[a[0]];
            if ((shm_flags(h) & O_ACCMODE) == O_RDONLY)
                return -EBADF;
            long r = a[8] == SYS_ftruncate ? ftruncate(shm_raw(h), a[1])
                                           : syscall(SYS_fallocate, shm_raw(h), a[1], a[2], a[3]);
            return r < 0 ? -errno : r;
        }
        if (a[8] == SYS_fallocate)
            return -ENOTSUP;
        if (a[0] >= GUEST_FDS || !state_fds[a[0]])
            return -EBADF;
        return ftruncate(host_descriptor(a[0]), a[1]) < 0 ? -errno : 0;
    }
    case SYS_mmap:
        if (tracing)
            fprintf(stderr,
                    "linux-run: mmap addr=%#lx len=%#lx prot=%lu flags=%#lx fd=%ld off=%#lx\n",
                    (unsigned long)a[0], (unsigned long)a[1], (unsigned long)a[2],
                    (unsigned long)a[3], (long)a[4], (unsigned long)a[5]);
        if (!(a[3] & MAP_ANONYMOUS) && a[4] < GUEST_FDS && shm_files[a[4]]) {
            struct shm_handle *h = shm_files[a[4]];
            int mode = shm_flags(h) & O_ACCMODE;
            if (mode == O_WRONLY ||
                (mode == O_RDONLY && (a[2] & PROT_WRITE) && (a[3] & MAP_TYPE) == MAP_SHARED))
                return -EACCES;
            return high_file(a[0], a[1], a[2], a[3], shm_raw(h), a[5]);
        }
        if (!(a[3] & MAP_ANONYMOUS) && a[4] < GUEST_FDS && state_fds[a[4]])
            return high_file(a[0], a[1], a[2], a[3], host_descriptor(a[4]), a[5]);
        if (!(a[3] & MAP_ANONYMOUS) && (a[4] >= GUEST_FDS || !files[a[4]]))
            return -EBADF;
        return map_linux(a[0], a[1], a[2], a[3], (a[3] & MAP_ANONYMOUS) ? NULL : files[a[4]], a[5]);
    case SYS_munmap:
    case SYS_mprotect:
        if (a[8] == SYS_mprotect && a[2] > 7)
            return -EINVAL;
        if (a[0] >= ARENA)
            return high_update(a[0], a[1], a[8] == SYS_mprotect ? (int)a[2] : -1);
        if (a[0] >= ARENA || a[1] > ARENA - a[0] || (a[8] == SYS_mprotect && a[2] > 7))
            return -EINVAL;
        return page_update(a[0], a[0] + a[1], a[8] == SYS_mprotect ? (int)a[2] : -1,
                           a[8] == SYS_mprotect);
    case SYS_madvise:
        if (a[2] != MADV_DONTNEED)
            return -ENOSYS;
        if (a[0] % PAGE)
            return -EINVAL;
        if (!a[1])
            return 0;
        return a[0] >= ARENA ? high_update(a[0], a[1], -2) : discard_pages(a[0], a[1]);
    case SYS_brk: {
        u64 brk = *memory_brk();
        if (a[0] >= BRK_BASE && a[0] < MMAP_BASE) {
            u64 from = (brk + PAGE - 1) & ~(u64)(PAGE - 1),
                to = (a[0] + PAGE - 1) & ~(u64)(PAGE - 1);
            if (to > from && set_mapping(from, to - from, PROT_READ | PROT_WRITE, NULL, 0, 0))
                return brk;
            if (to < from)
                page_update(to, from, -1, 0);
            *memory_brk() = brk = a[0];
        }
        return brk;
    }
    case SYS_getrandom: {
        void *p = guest_ptr(a[0], a[1], PROT_WRITE);
        if (!p)
            return -EFAULT;
        ssize_t n = getrandom(p, a[1], a[2]);
        return n < 0 ? -errno : n;
    }
    case SYS_set_tid_address:
        return thread_set_tid(a[0]);
    default:
        return -ENOSYS;
    }
}

void files_init(void) { guest_process->descriptors = &initial_fds; }
void files_destroy(struct guest_process *process) {
    struct guest_files *table = process->descriptors;
    if (!table)
        return;
    for (unsigned i = 0; i < GUEST_FDS; i++)
        if (table->ranges[i]) release_ranges(table, i, process->pid);
    for (unsigned i = 0; i < GUEST_FDS; i++) {
        shm_drop(table->shm[i]);
        proc_close(table->proc[i]);
        if (table->libraries[i])
            close_backing(table->libraries[i]);
        if (table->native[i])
            release_raw(table->native[i] - 1);
        if (table->flocks[i])
            close(table->flocks[i] - 1);
        if (table->records[i])
            close(table->records[i] - 1);
    }
    if (table != &initial_fds)
        free(table);
    process->descriptors = NULL;
}
static int inherit_native(int encoded, int *destination) {
    if (!encoded)
        return 0;
    int flags = fcntl(encoded - 1, F_GETFD);
    if (flags < 0)
        return -errno;
    int fd = fcntl(encoded - 1, flags & FD_CLOEXEC ? F_DUPFD_CLOEXEC : F_DUPFD, 0);
    if (fd < 0)
        return -errno;
    *destination = fd + 1;
    return 0;
}
int files_clone(struct guest_process *child) {
    struct guest_files *src = fd_table();
    struct guest_files *table = calloc(1, sizeof *table);
    if (!table)
        return -ENOMEM;
    child->descriptors = table;
    memcpy(table->std_closed, src->std_closed, sizeof table->std_closed);
    memcpy(table->cloexec, src->cloexec, sizeof table->cloexec);
    memcpy(table->actions, src->actions, sizeof table->actions);
    memcpy(table->devices, src->devices, sizeof table->devices);
    memcpy(table->sockets, src->sockets, sizeof table->sockets);
    memcpy(table->paths, src->paths, sizeof table->paths);
    for (unsigned i = 0; i < GUEST_FDS; i++) {
        table->shm[i] = src->shm[i];
        shm_retain(table->shm[i]);
        table->proc[i] = src->proc[i];
        proc_inherit(table->proc[i]);
        table->libraries[i] = src->libraries[i];
        if (table->libraries[i]) {
            ++table->libraries[i]->refs;
            ++table->libraries[i]->open;
        }
        if (src->native[i])
            table->native[i] = retain_raw(src->native[i] - 1) + 1;
        int rc = inherit_native(src->flocks[i], &table->flocks[i]);
        if (rc)
            return rc;
    }
    return 0;
}
