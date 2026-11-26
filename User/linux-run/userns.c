// User-namespace IDs are translated only inside the interpreter. All native
// I/O continues under the original unprivileged LixinOS identity.
#include "runtime.h"
#include <ctype.h>
#include <dirent.h>
#include <stddef.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum {
    PROC_ROOT = 1,
    TASKS,
    FD_DIR,
    FDINFO,
    TASK_DIR,
    EMPTY_ROOT,
    UID_MAP,
    GID_MAP,
    SETGROUPS,
    USER_NS,
    PID_NS,
    NET_NS,
    TASK_STATUS
};
// CLONE_FS shares this view, including the irreversible switch to the
// restricted root. Ordinary fork inherits a private copy.
struct guest_fs {
    unsigned refs;
    int confined;
};
static struct guest_fs initial_fs = {.refs = 1};
static struct guest_fs *view(void) { return guest_process->fs ? guest_process->fs : &initial_fs; }
int fs_clone(struct guest_process *child, int share) {
    if (share) {
        child->fs = view();
        ++child->fs->refs;
    } else {
        child->fs = malloc(sizeof *child->fs);
        if (!child->fs)
            return -ENOMEM;
        *child->fs = (struct guest_fs){.refs = 1, .confined = view()->confined};
    }
    return 0;
}
void fs_put(struct guest_process *p) {
    if (p->fs && !--p->fs->refs && p->fs != &initial_fs)
        free(p->fs);
    p->fs = NULL;
}
int fs_confined(void) { return view()->confined; }
long fs_chroot(const char *path) {
    if (!(thread_effective_caps() & (1u << 18)))
        return -EPERM;
    if (fs_confined() || (strcmp(path, "/proc/self/fdinfo/") && strcmp(path, "/proc/self/fdinfo")))
        return -ENOENT;
    view()->confined = 1;
    return 0;
}
long fs_chdir(const char *path) { return !strcmp(path, "/") || !strcmp(path, ".") ? 0 : -ENOENT; }
static int directory_node(int node) { return node >= PROC_ROOT && node <= EMPTY_ROOT; }

struct mapping {
    unsigned inside, outside;
    int written;
};
struct user_namespace {
    unsigned refs, depth;
    struct mapping uid, gid;
    int deny_groups;
};
struct proc_handle {
    struct user_namespace *ns;
    unsigned refs;
    int node, mode;
    struct proc_task task;
    size_t pos;
};

unsigned namespace_id(int group) {
    struct user_namespace *ns = guest_process->userns;
    if (!ns)
        return 1000;
    struct mapping *m = group ? &ns->gid : &ns->uid;
    return m->written ? m->inside : 65534;
}
static void ns_ref(struct user_namespace *ns) {
    if (ns)
        ++ns->refs;
}
void namespace_put(struct user_namespace *ns) {
    if (ns && !--ns->refs)
        free(ns);
}
int namespace_clone(struct guest_process *child, int create) {
    struct user_namespace *old = guest_process->userns;
    if (!create) {
        ns_ref(old);
        child->userns = old;
        return 0;
    }
    if (old && (!old->uid.written || !old->gid.written))
        return -EPERM;
    if (old && old->depth >= 32)
        return -ENOSPC;
    struct user_namespace *ns = calloc(1, sizeof *ns);
    if (!ns)
        return -ENOMEM;
    ns->refs = 1;
    ns->depth = old ? old->depth + 1 : 1;
    ns->uid.outside = namespace_id(0);
    ns->gid.outside = namespace_id(1);
    ns->deny_groups = old && old->deny_groups;
    child->userns = ns;
    return 0;
}
long namespace_unshare(u64 flags) {
    if (fs_confined())
        return -EPERM;
    if (flags != CLONE_NEWUSER)
        return -EINVAL;
    if (guest_process->threads != 1)
        return -EINVAL;
    struct user_namespace *old = guest_process->userns;
    int rc = namespace_clone(guest_process, 1);
    if (!rc) {
        namespace_put(old);
        thread_new_namespace();
    }
    return rc;
}
static int decimal_id(const char **p) {
    if (**p < '1' || **p > '9')
        return 0;
    char *end;
    unsigned long id = strtoul(*p, &end, 10);
    *p = end;
    return id <= INT_MAX ? (int)id : 0;
}
static int task_node(const char *suffix, struct proc_task *task) {
    if (!*suffix)
        return TASKS;
    task->tid = decimal_id(&suffix);
    if (!task->tid || process_next_tid(*task, task->tid - 1) != task->tid)
        return 0;
    if (!*suffix || !strcmp(suffix, "/"))
        return TASK_DIR;
    return !strcmp(suffix, "/status") ? TASK_STATUS : 0;
}
int proc_node(const char *path, struct proc_handle *dir, struct proc_task *task) {
    *task = (struct proc_task){.owner = guest_process->pid, .depth = guest_process->pids.depth};
    if (fs_confined() && (*path == '/' || !dir))
        return !strcmp(path, "/") || !strcmp(path, ".") || !strcmp(path, "..") ? EMPTY_ROOT : 0;
    if (*path == '/') {
        if (!strcmp(path, "/proc") || !strcmp(path, "/proc/"))
            return PROC_ROOT;
        if (strncmp(path, "/proc/", 6))
            return 0;
        path += 6;
    } else if (dir && directory_node(dir->node)) {
        if (!strcmp(path, ".")) {
            *task = dir->task;
            return dir->node;
        }
        if (dir->node == TASKS || dir->node == TASK_DIR) {
            *task = dir->task;
            if (!process_thread_count(task->owner))
                return 0;
            if (dir->node == TASKS)
                return task_node(path, task);
            return !strcmp(path, "status") &&
                   process_next_tid(*task, task->tid - 1) == task->tid ? TASK_STATUS : 0;
        }
        if (dir->node != PROC_ROOT)
            return 0;
    } else {
        return 0;
    }
    if (*path >= '1' && *path <= '9') {
        int pid = decimal_id(&path);
        if (!pid || !(task->owner = process_for_proc(pid)) || strncmp(path, "/task", 5))
            return 0;
        path += 5;
        return !*path ? TASKS : *path == '/' ? task_node(path + 1, task) : 0;
    }
    if (!strncmp(path, "self/task/", 10))
        return task_node(path + 10, task);
    if (!strcmp(path, "self/fd") || !strcmp(path, "self/fd/"))
        return FD_DIR;
    if (!strcmp(path, "self/fdinfo") || !strcmp(path, "self/fdinfo/"))
        return FDINFO;
    if (!strcmp(path, "self/task") || !strcmp(path, "self/task/"))
        return TASKS;
    if (!strcmp(path, "self/uid_map"))
        return UID_MAP;
    if (!strcmp(path, "self/gid_map"))
        return GID_MAP;
    if (!strcmp(path, "self/setgroups"))
        return SETGROUPS;
    if (independent_processes && !strcmp(path, "self/ns/pid"))
        return PID_NS;
    if (independent_processes && !strcmp(path, "self/ns/net"))
        return NET_NS;
    if (!strcmp(path, "self/ns/user"))
        return USER_NS;
    return 0;
}
int proc_open(int node, struct proc_task task, int flags, struct proc_handle **out) {
    int directory = directory_node(node);
    int mode = flags & O_ACCMODE;
    if ((flags & O_DIRECTORY) && !directory)
        return -ENOTDIR;
    if (directory && mode != O_RDONLY)
        return -EISDIR;
    if (flags & ~(O_CLOEXEC | O_DIRECTORY | O_ACCMODE | O_LARGEFILE | O_NOFOLLOW | O_NONBLOCK))
        return -EINVAL;
    if (mode == 3 || (node >= USER_NS && mode != O_RDONLY))
        return -EACCES;
    struct proc_handle *h = calloc(1, sizeof *h);
    if (!h)
        return -ENOMEM;
    h->node = node;
    h->task = task;
    h->mode = mode;
    h->refs = 1;
    h->ns = guest_process->userns;
    ns_ref(h->ns);
    *out = h;
    return 0;
}
void proc_inherit(struct proc_handle *h) {
    if (h)
        ++h->refs;
}
void proc_close(struct proc_handle *h) {
    if (h && !--h->refs) {
        namespace_put(h->ns);
        free(h);
    }
}
void proc_stat(int node, struct proc_task task, struct stat *st) {
    memset(st, 0, sizeof *st);
    int dir = directory_node(node);
    st->st_mode = dir ? S_IFDIR | 0555 : S_IFREG | (node == TASK_STATUS ? 0444 : 0644);
    st->st_nlink = node == TASKS ? 2 + process_thread_count(task.owner) : dir ? 2 : 1;
    st->st_uid = namespace_id(0);
    st->st_gid = namespace_id(1);
}
int proc_flags(struct proc_handle *h) {
    return h->mode | (directory_node(h->node) ? O_DIRECTORY : 0);
}
void proc_fstat(struct proc_handle *h, struct stat *st) { proc_stat(h->node, h->task, st); }
int proc_target_fd(struct proc_handle *dir, const char *name) {
    if (!dir || dir->node != FD_DIR || !*name)
        return -1;
    unsigned fd = 0;
    for (const char *c = name; *c; c++) {
        if (*c < '0' || *c > '9')
            return -1;
        fd = 10 * fd + (*c - '0');
        if (fd >= GUEST_FDS)
            return -1;
    }
    return fd;
}
long proc_entries(struct proc_handle *h, u64 addr, size_t bytes) {
    if (h->node != FD_DIR && h->node != FDINFO && h->node != EMPTY_ROOT &&
        h->node != TASKS && h->node != TASK_DIR)
        return directory_node(h->node) ? -ENOTSUP : -ENOTDIR;
    struct record {
        u64 ino, offset;
        unsigned short size;
        unsigned char type;
        char name[16];
    };
    size_t limit = h->node == TASKS ? (size_t)INT_MAX + 3 :
                   h->node == EMPTY_ROOT || h->node == TASK_DIR ? 2 : GUEST_FDS + 2;
    size_t total = 0;
    while (h->pos < limit) {
        size_t pos = h->pos;
        if (pos >= 2 && h->node == TASKS) {
            int tid = process_next_tid(h->task, (int)(pos - 2));
            if (!tid)
                break;
            pos = (size_t)tid + 1;
        } else if (pos >= 2 && !fd_occupied(pos - 2)) {
            ++h->pos;
            continue;
        }
        struct record entry = {.ino = pos + 1, .offset = pos + 1};
        entry.type = pos < 2 || h->node == TASKS ? DT_DIR : h->node == FD_DIR ? DT_LNK : DT_REG;
        if (pos < 2)
            strcpy(entry.name, pos ? ".." : ".");
        else if (h->node == TASKS)
            snprintf(entry.name, sizeof entry.name, "%u", (unsigned)(pos - 1));
        else
            snprintf(entry.name, sizeof entry.name, "%u", (unsigned)(pos - 2));
        entry.size = (offsetof(struct record, name) + strlen(entry.name) + 8) & ~7u;
        if (entry.size > bytes - total)
            return total ? (long)total : -EINVAL;
        void *out = guest_ptr(addr + total, entry.size, PROT_WRITE);
        if (!out)
            return total ? (long)total : -EFAULT;
        memcpy(out, &entry, entry.size);
        total += entry.size;
        h->pos = pos + 1;
    }
    return total;
}
static int mapping_values(char *p, unsigned values[3]) {
    for (int i = 0; i < 3; ++i) {
        while (isspace((unsigned char)*p))
            ++p;
        if (!isdigit((unsigned char)*p))
            return 0;
        char *end;
        unsigned long v = strtoul(p, &end, 10);
        if (v >= UINT_MAX || (*end && !isspace((unsigned char)*end)))
            return 0;
        values[i] = v;
        p = end;
    }
    while (isspace((unsigned char)*p))
        ++p;
    return !*p;
}
long proc_io(struct proc_handle *h, u64 addr, size_t len, int writing) {
    if (directory_node(h->node))
        return -EISDIR;
    if ((writing && h->mode == O_RDONLY) || (!writing && h->mode == O_WRONLY))
        return -EBADF;
    struct user_namespace *ns = h->ns;
    char buf[512];
    struct mapping *m = !ns ? NULL : h->node == GID_MAP ? &ns->gid : &ns->uid;
    if (writing) {
        if (!ns || ns != guest_process->userns || h->node >= USER_NS)
            return -EPERM;
        unsigned required = h->node == UID_MAP ? 7 : 6;
        if (!(thread_effective_caps() & (1u << required)))
            return -EPERM;
        if (h->pos || !len || len >= sizeof buf)
            return -EINVAL;
        const void *in = guest_ptr(addr, len, PROT_READ);
        if (!in)
            return -EFAULT;
        memcpy(buf, in, len);
        if (memchr(buf, 0, len))
            return -EINVAL;
        buf[len] = 0;
        if (h->node == SETGROUPS) {
            if (ns->gid.written || (strcmp(buf, "deny") && strcmp(buf, "deny\n")))
                return -EPERM;
            ns->deny_groups = 1;
        } else {
            unsigned ids[3];
            if (!mapping_values(buf, ids))
                return -EINVAL;
            if (m->written || ids[2] != 1 || ids[1] != m->outside ||
                (h->node == GID_MAP && !ns->deny_groups))
                return -EPERM;
            m->inside = ids[0];
            m->written = 1;
        }
        h->pos += len;
        return len;
    }
    int n;
    if (h->node == TASK_STATUS) {
        n = process_thread_status(h->task, buf, sizeof buf);
        if (n < 0)
            return n;
    } else if (h->node == SETGROUPS)
        n = snprintf(buf, sizeof buf, "%s\n", ns && ns->deny_groups ? "deny" : "allow");
    else if (h->node >= USER_NS)
        return -EINVAL;
    else if (!ns)
        n = snprintf(buf, sizeof buf, "0 0 4294967295\n");
    else if (m->written)
        n = snprintf(buf, sizeof buf, "%u %u 1\n", m->inside, m->outside);
    else
        n = 0;
    if (h->pos >= (size_t)n || !len)
        return 0;
    if (len > n - h->pos)
        len = n - h->pos;
    void *out = guest_ptr(addr, len, PROT_WRITE);
    if (!out)
        return -EFAULT;
    memcpy(out, buf + h->pos, len);
    h->pos += len;
    return len;
}
