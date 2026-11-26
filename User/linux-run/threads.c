// Linux CPU contexts run on detached LixinOS pthreads. Only one context at a
// time may change the interpreter's page table or translated descriptor table.
#include "runtime.h"
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

struct linux_thread {
    CPU cpu;
    struct linux_thread *next;
    struct guest_process *process;
    int tid;
    int *futex_word;
    int futex_result;
    char name[16];
    u64 clear_tid;
    u64 mask, saved_poll_mask;
    int restore_poll_mask;
    unsigned caps_effective, caps_permitted;
    int no_new_privs;
    u64 accounted, cpu_ticks;
};

static struct guest_process root_process = {.data_soft = UINT64_MAX, .data_hard = UINT64_MAX,
                                            .nofile_soft = GUEST_FDS, .nofile_hard = GUEST_FDS};
struct guest_process *guest_process = &root_process;
static struct guest_process *processes;
static struct linux_thread leader = {.process = &root_process};
static struct linux_thread *current, *active_threads;
static pthread_mutex_t cpu_lock = PTHREAD_MUTEX_INITIALIZER;
static int threads_alive, next_tid;
extern void lx_schedule(void);
void thread_slice(CPU *cpu) {
    if (threads_alive > 1) {
        thread_pause();
        lx_schedule();
        thread_resume(cpu);
    }
}

CPU *thread_cpu(void) { return current ? &current->cpu : &leader.cpu; }
void thread_resume(CPU *c) {
    pthread_mutex_lock(&cpu_lock);
    current = (struct linux_thread *)c;
    guest_process = current->process;
    if (guest_process->exiting)
        thread_exit(guest_process->status);
}
// Register under the guest CPU lock and sleep on a private native word. An
// exit or signal can then wake this thread without a race at the sleep boundary.
static void finish_futex(struct linux_thread *t, int result) {
    t->futex_word = NULL;
    __atomic_store_n(&t->futex_result, result, __ATOMIC_RELEASE);
    syscall(SYS_futex, &t->futex_result, 1, 1, NULL, NULL, 0);
}
int thread_futex_wake(int *word, int count) {
    if (count < 0)
        return -EINVAL;
    int woken = 0;
    for (struct linux_thread *t = active_threads; t && woken < count; t = t->next) {
        if (t->futex_word == word) {
            finish_futex(t, 1);
            ++woken;
        }
    }
    return woken;
}
static void interrupt_futexes(struct guest_process *p, int sig) {
    for (struct linux_thread *t = active_threads; t; t = t->next) {
        if (t->process == p && t->futex_word && (!sig || !(t->mask & (1ULL << (sig - 1))))) {
            finish_futex(t, -EINTR);
            if (sig)
                break;
        }
    }
}
long thread_futex_wait(CPU *cpu, int *word, int expected, int op, const struct timespec *timeout) {
    if (__atomic_load_n(word, __ATOMIC_RELAXED) != expected)
        return -EAGAIN;
    if (signal_ready())
        return -EINTR;
    struct linux_thread *t = current;
    t->futex_result = 0;
    t->futex_word = word;
    thread_pause();
    long rc = syscall(SYS_futex, &t->futex_result, op, 0, timeout, NULL, ~0u);
    int error = errno;
    thread_resume(cpu);
    t->futex_word = NULL;
    if (t->futex_result)
        return t->futex_result == 1 ? 0 : t->futex_result;
    return rc < 0 ? -error : rc;
}
static void account_cpu(void) {
    u64 elapsed = current->cpu.icount - current->accounted;
    current->accounted = current->cpu.icount;
    current->cpu_ticks += elapsed;
    current->process->cpu_ticks += elapsed;
}
// The emulated generic timer advances one tick per retired instruction.
void thread_cputime(int process, struct timespec *out) {
    account_cpu();
    u64 ticks = process ? guest_process->cpu_ticks : current->cpu_ticks;
    u64 hz = current->cpu.cntfrq;
    out->tv_sec = ticks / hz;
    out->tv_nsec = (ticks % hz) * 1000000000ULL / hz;
}
void thread_pause(void) {
    if (current) {
        account_cpu();
        current->cpu.excl_valid = false;
    }
    pthread_mutex_unlock(&cpu_lock);
}

CPU *start_threads(void) {
    thread_resume(&leader.cpu);
    root_process.pid = leader.tid = getpid();
    root_process.ppid = getppid();
    root_process.threads = 1;
    processes = &root_process;
    next_tid = leader.tid;
    active_threads = &leader;
    threads_alive = 1;
    return &leader.cpu;
}

static void *start_worker(void *arg) {
    struct linux_thread *t = arg;
    thread_resume(&t->cpu);
    interpret(&t->cpu);
    return NULL;
}

static void release_process(struct guest_process *p) {
    fs_put(p);
    namespace_put(p->userns);
    events_destroy(p);
    files_destroy(p);
    upper_destroy(p);
    memory_destroy(p);
}

static void reap(struct guest_process *p) {
    struct guest_process **link = &processes;
    while (*link != p)
        link = &(*link)->next;
    *link = p->next;
    free(p);
}

int namespace_pid(const struct pid_namespace *viewer, const struct pid_namespace *owner,
                  int global) {
    if (!viewer->depth)
        return global;
    unsigned index = viewer->depth - 1;
    if (owner->depth <= index || owner->init[index] != viewer->init[index])
        return 0;
    return global == viewer->init[index] ? 1 : global;
}
int process_pid(void) {
    return namespace_pid(&guest_process->pids, &guest_process->pids, guest_process->pid);
}
int process_ppid(void) {
    if (!guest_process->pids.depth)
        return guest_process->ppid;
    for (struct guest_process *p = processes; p; p = p->next)
        if (p->pid == guest_process->ppid)
            return namespace_pid(&guest_process->pids, &p->pids, p->pid);
    return 0;
}
long process_wait(CPU *c) {
    int by_id = c->x[8] == SYS_waitid;
    int pid = by_id ? (int)c->x[1] : (int)c->x[0];
    int options = by_id ? (int)c->x[3] : (int)c->x[2];
    if (by_id) {
        if (c->x[0] == P_ALL)
            pid = -1;
        else if (c->x[0] != P_PID || pid <= 0)
            return -EINVAL;
        if (!(options & WEXITED) || (options & ~(WEXITED | WNOHANG | WNOWAIT)) || c->x[4])
            return -ENOTSUP;
    } else if (options & ~WNOHANG || c->x[3] || pid == 0 || pid < -1) {
        return -ENOTSUP;
    }
    for (;;) {
        int children = 0;
        for (struct guest_process *p = processes; p; p = p->next) {
            int visible = namespace_pid(&guest_process->pids, &p->pids, p->pid);
            if (p->ppid != guest_process->pid || (pid > 0 && visible != pid))
                continue;
            children = 1;
            if (p->threads)
                continue;
            if (by_id && c->x[2]) {
                siginfo_t info = {0};
                info.si_signo = SIGCHLD;
                info.si_code = p->term_signal ? CLD_KILLED : CLD_EXITED;
                info.si_pid = visible;
                info.si_uid = namespace_id(0);
                info.si_status = p->term_signal ? p->term_signal : p->status & 255;
                void *out = guest_ptr(c->x[2], sizeof info, PROT_WRITE);
                if (!out)
                    return -EFAULT;
                memcpy(out, &info, sizeof info);
            } else if (!by_id && c->x[1]) {
                int status = p->term_signal ? p->term_signal : (p->status & 255) << 8;
                void *out = guest_ptr(c->x[1], sizeof status, PROT_WRITE);
                if (!out)
                    return -EFAULT;
                memcpy(out, &status, sizeof status);
            }
            if (!(options & WNOWAIT))
                reap(p);
            return by_id ? 0 : visible;
        }
        if (!children)
            return -ECHILD;
        if (options & WNOHANG) {
            if (by_id && c->x[2]) {
                void *out = guest_ptr(c->x[2], sizeof(siginfo_t), PROT_WRITE);
                if (!out)
                    return -EFAULT;
                memset(out, 0, sizeof(siginfo_t));
            }
            return 0;
        }
        thread_pause();
        lx_schedule();
        thread_resume(c);
    }
}

static int launch_worker(struct linux_thread *t) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t host;
    int e = pthread_create(&host, &attr, start_worker, t);
    pthread_attr_destroy(&attr);
    if (e)
        return -e;
    t->next = active_threads;
    active_threads = t;
    ++threads_alive;
    ++t->process->threads;
    return 0;
}

static long fork_guest(CPU *parent) {
    const u64 *a = parent->x;
    unsigned allowed = CLONE_VM | CLONE_FS | CLONE_VFORK | CLONE_SETTLS | CLONE_NEWPID |
                       CLONE_NEWNET | CLONE_NEWUSER | CLONE_PARENT_SETTID | CLONE_CHILD_SETTID |
                       CLONE_CHILD_CLEARTID;
    if ((a[0] & 255) != SIGCHLD || (a[0] & ~(u64)(allowed | 255)))
        return -ENOSYS;
    int shared_vm = !!(a[0] & CLONE_VM);
    if (shared_vm != !!(a[0] & CLONE_VFORK) || (shared_vm && !a[1]))
        return -ENOTSUP;
    if ((a[0] & (CLONE_NEWNET | CLONE_NEWPID)) && !(a[0] & CLONE_NEWUSER) &&
        !(current->caps_effective & (1u << 21)))
        return -EPERM;
    if ((a[0] & CLONE_NEWPID) && guest_process->pids.depth == 32)
        return -ENOSPC;
    if (a[1] && (a[1] % 16 || !guest_ptr(a[1] - 1, 1, PROT_WRITE)))
        return -EINVAL;
    void *parent_tid = NULL;
    if (a[0] & CLONE_PARENT_SETTID) {
        parent_tid = guest_ptr(a[2], sizeof(int), PROT_WRITE);
        if (!parent_tid)
            return -EFAULT;
    }
    if ((a[0] & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) &&
        !guest_ptr(a[4], sizeof(int), PROT_WRITE))
        return -EFAULT;
    if (next_tid == INT_MAX)
        return -EAGAIN;
    struct guest_process *p = calloc(1, sizeof *p);
    struct linux_thread *t = calloc(1, sizeof *t);
    if (!p || !t) {
        free(p);
        free(t);
        return -ENOMEM;
    }
    memcpy(t->name, current->name, sizeof t->name);
    p->data_soft = guest_process->data_soft;
    p->data_hard = guest_process->data_hard;
    p->nofile_soft = guest_process->nofile_soft;
    p->nofile_hard = guest_process->nofile_hard;
    p->vfork_active = shared_vm;
    p->sigreturn_pc = guest_process->sigreturn_pc;
    p->ppid = guest_process->pid;
    p->pid = t->tid = ++next_tid;
    p->pids = guest_process->pids;
    if (a[0] & CLONE_NEWPID)
        p->pids.init[p->pids.depth++] = p->pid;
    p->net_namespace = a[0] & CLONE_NEWNET ? p->pid : guest_process->net_namespace;
    t->process = p;
    t->cpu = *parent;
    t->accounted = t->cpu.icount;
    t->mask = current->mask;
    t->cpu.x[0] = 0;
    t->cpu.excl_valid = false;
    if (a[1])
        *cpu_cur_sp(&t->cpu) = a[1];
    if (a[0] & CLONE_CHILD_CLEARTID)
        t->clear_tid = a[4];
    t->no_new_privs = current->no_new_privs;
    t->caps_permitted = current->caps_permitted;
    t->caps_effective = current->caps_effective;
    int rc = namespace_clone(p, !!(a[0] & CLONE_NEWUSER));
    if (!rc && (a[0] & CLONE_NEWUSER))
        t->caps_effective = t->caps_permitted = (1u << 6) | (1u << 7) | (1u << 18) | (1u << 21);
    if (!rc)
        rc = fs_clone(p, a[0] & CLONE_FS);
    if (!rc && shared_vm) {
        memory_share(p);
        upper_share(p);
    } else if (!rc) {
        rc = memory_clone(p);
        if (!rc)
            rc = upper_clone(p);
    }
    if (a[0] & CLONE_SETTLS)
        t->cpu.tpidr[0] = a[3];
    if (!rc)
        rc = files_clone(p);
    if (!rc)
        rc = events_clone(p);
    if (!rc && (a[0] & CLONE_CHILD_SETTID)) {
        struct guest_process *saved = guest_process;
        guest_process = p;
        void *out = guest_ptr(a[4], sizeof(int), PROT_WRITE);
        int local = process_pid();
        if (out)
            memcpy(out, &local, sizeof local);
        else
            rc = -EFAULT;
        guest_process = saved;
    }
    if (!rc)
        rc = launch_worker(t);
    if (rc) {
        release_process(p);
        free(p);
        free(t);
        return rc;
    }
    p->next = processes;
    processes = p;
    if (parent_tid)
        memcpy(parent_tid, &p->pid, sizeof(int));
    int child = p->pid;
    while (shared_vm) {
        struct guest_process *live = processes;
        while (live && live->pid != child)
            live = live->next;
        if (!live || !live->threads || !live->vfork_active)
            break;
        thread_pause();
        lx_schedule();
        thread_resume(parent);
    }
    return child;
}

long clone_thread(CPU *parent) {
    const u64 *a = parent->x;
    const unsigned shared = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD;
    const unsigned supported = shared | CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID |
                               CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID;
    if ((a[0] & CLONE_NEWUSER) && (a[0] & (CLONE_FS | CLONE_THREAD)))
        return -EINVAL;
    if (!(a[0] & CLONE_THREAD)) {
        if (!independent_processes)
            return a[0] & CLONE_NEWUSER ? -EINVAL : -ENOSYS;
        return fork_guest(parent);
    }
    if ((a[0] & shared) != shared || (a[0] & ~(u64)supported))
        return -ENOSYS;
    if (!a[1] || a[1] % 16 || !guest_ptr(a[1] - 1, 1, PROT_WRITE))
        return -EINVAL;
    int *ptid = NULL, *ctid = NULL;
    if (a[0] & CLONE_PARENT_SETTID) {
        ptid = guest_ptr(a[2], sizeof *ptid, PROT_WRITE);
        if (!ptid)
            return -EFAULT;
    }
    if (a[0] & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) {
        ctid = guest_ptr(a[4], sizeof *ctid, PROT_WRITE);
        if (!ctid)
            return -EFAULT;
    }
    if (next_tid == INT_MAX)
        return -EAGAIN;
    struct linux_thread *t = malloc(sizeof *t);
    if (!t)
        return -ENOMEM;
    *t = *current;
    t->accounted = parent->icount;
    t->cpu_ticks = 0;
    t->tid = ++next_tid;
    t->clear_tid = 0;
    t->cpu.x[0] = 0;
    t->cpu.excl_valid = false;
    *cpu_cur_sp(&t->cpu) = a[1];
    if (a[0] & CLONE_SETTLS)
        t->cpu.tpidr[0] = a[3];
    if (a[0] & CLONE_CHILD_CLEARTID)
        t->clear_tid = a[4];
    int rc = launch_worker(t);
    if (rc) {
        free(t);
        return rc;
    }
    if (ptid)
        memcpy(ptid, &t->tid, sizeof *ptid);
    if (a[0] & CLONE_CHILD_SETTID)
        memcpy(ctid, &t->tid, sizeof *ctid);
    return t->tid;
}

void thread_new_namespace(void) {
    current->caps_effective = current->caps_permitted =
        (1u << 6) | (1u << 7) | (1u << 18) | (1u << 21);
}
unsigned thread_effective_caps(void) { return current->caps_effective; }
void thread_exec_name(const char *path) {
    const char *slash = strrchr(path, '/');
    snprintf(current->name, sizeof current->name, "%.15s", slash ? slash + 1 : path);
}
long thread_caps(CPU *cpu) {
    const u64 *a = cpu->x;
    if (a[8] == SYS_prctl) {
        if (a[0] == PR_GET_NAME)
            return guest_copy(a[1], current->name, sizeof current->name, 1) ? 0 : -EFAULT;
        if (a[0] == PR_SET_NAME) {
            char name[sizeof current->name] = {0};
            for (size_t i = 0; i + 1 < sizeof name; i++) {
                if (!guest_copy(a[1] + i, &name[i], 1, 0)) return -EFAULT;
                if (!name[i]) break;
            }
            memcpy(current->name, name, sizeof name);
            return 0;
        }
        if (a[0] == PR_GET_NO_NEW_PRIVS || a[0] == PR_SET_NO_NEW_PRIVS) {
            if (a[2] || a[3] || a[4])
                return -EINVAL;
            if (a[0] == PR_GET_NO_NEW_PRIVS)
                return a[1] ? -EINVAL : current->no_new_privs;
            if (a[1] != 1)
                return -EINVAL;
            current->no_new_privs = 1;
            return 0;
        }
        return -EINVAL;
    }
    struct {
        unsigned version;
        int pid;
    } header;
    void *ptr = guest_ptr(a[0], sizeof header, PROT_READ);
    if (!ptr)
        return -EFAULT;
    memcpy(&header, ptr, sizeof header);
    if (header.version != 0x20080522 && header.version != 0x20071026 &&
        header.version != 0x19980330) {
        unsigned version = 0x20080522;
        ptr = guest_ptr(a[0], sizeof version, PROT_WRITE);
        if (!ptr)
            return -EFAULT;
        memcpy(ptr, &version, sizeof version);
        return -EINVAL;
    }
    if (header.pid && header.pid != thread_tid())
        return -EPERM;
    unsigned caps[6] = {0};
    size_t bytes = header.version == 0x19980330 ? 12 : sizeof caps;
    int get = a[8] == SYS_capget;
    ptr = guest_ptr(a[1], bytes, get ? PROT_WRITE : PROT_READ);
    if (!ptr)
        return -EFAULT;
    if (get) {
        caps[0] = current->caps_effective;
        caps[1] = current->caps_permitted;
        memcpy(ptr, caps, bytes);
        return 0;
    }
    memcpy(caps, ptr, bytes);
    if ((caps[1] & ~current->caps_permitted) || (caps[0] & ~caps[1]) || caps[2] || caps[3] ||
        caps[4] || caps[5])
        return -EPERM;
    current->caps_effective = caps[0];
    current->caps_permitted = caps[1];
    return 0;
}
int thread_tid(void) {
    return namespace_pid(&guest_process->pids, &guest_process->pids, current->tid);
}
int process_for_proc(int pid) {
    for (struct guest_process *p = processes; p; p = p->next)
        if (p->threads && namespace_pid(&guest_process->pids, &p->pids, p->pid) == pid)
            return p->pid;
    return 0;
}
int process_thread_count(int owner) {
    for (struct guest_process *p = processes; p; p = p->next)
        if (p->pid == owner)
            return p->threads;
    return 0;
}
static int proc_visible_tid(const struct pid_namespace *ns, unsigned depth, int tid) {
    return depth && ns->init[depth - 1] == tid ? 1 : tid;
}
int process_next_tid(struct proc_task task, int after) {
    int next = 0;
    for (struct linux_thread *t = active_threads; t; t = t->next) {
        if (t->process->pid != task.owner)
            continue;
        int tid = proc_visible_tid(&t->process->pids, task.depth, t->tid);
        if (tid > after && (!next || tid < next))
            next = tid;
    }
    return next;
}
int process_thread_status(struct proc_task task, char *buf, size_t size) {
    for (struct linux_thread *t = active_threads; t; t = t->next) {
        const struct pid_namespace *ns = &t->process->pids;
        if (t->process->pid != task.owner || proc_visible_tid(ns, task.depth, t->tid) != task.tid)
            continue;
        int n = snprintf(buf, size, "Tgid:\t%d\nPid:\t%d\nNSpid:",
                         proc_visible_tid(ns, task.depth, task.owner), task.tid);
        for (unsigned depth = task.depth; depth <= ns->depth && (size_t)n < size; ++depth)
            n += snprintf(buf + n, size - n, "\t%d", proc_visible_tid(ns, depth, t->tid));
        if ((size_t)n + 2 > size)
            return -EOVERFLOW;
        buf[n++] = '\n';
        buf[n] = 0;
        return n;
    }
    return -ESRCH;
}
// Guest threads share the cooperative, normal-priority interpreter scheduler.
long thread_sched_query(int tid, int policy, u64 param) {
    if (tid < 0)
        return -EINVAL;
    if (tid && tid != thread_tid()) {
        struct linux_thread *t = active_threads;
        while (t && namespace_pid(&guest_process->pids, &t->process->pids, t->tid) != tid)
            t = t->next;
        if (!t)
            return -ESRCH;
    }
    if (policy)
        return SCHED_OTHER;
    int *priority = guest_ptr(param, sizeof *priority, PROT_WRITE);
    if (!priority)
        return -EFAULT;
    *priority = 0;
    return 0;
}
long thread_set_tid(u64 addr) {
    current->clear_tid = addr;
    return thread_tid();
}
long thread_mask(int how, u64 set, u64 old, u64 size) {
    if (size != 8)
        return -EINVAL;
    u64 mask = 0;
    if (set) {
        void *p = guest_ptr(set, 8, PROT_READ);
        if (!p)
            return -EFAULT;
        memcpy(&mask, p, 8);
        if (how < 0 || how > 2)
            return -EINVAL;
        mask &= ~((1ULL << 8) | (1ULL << 18)); // SIGKILL and SIGSTOP cannot be masked.
    }
    if (old) {
        void *p = guest_ptr(old, 8, PROT_WRITE);
        if (!p)
            return -EFAULT;
        memcpy(p, &current->mask, 8);
    }
    if (set) {
        if (!how)
            current->mask |= mask;
        else if (how == 1)
            current->mask &= ~mask;
        else
            current->mask = mask;
    }
    return 0;
}
_Noreturn void process_exit(int code) {
    if (tracing) fprintf(stderr, "linux-run: pid=%d exit_group(%d)\n", guest_process->pid, code);
    if (!current)
        exit(code);
    // With no other live processes, exit_group can terminate the interpreter
    // directly, including sibling threads parked in an indefinite futex wait.
    if (guest_process == &root_process && guest_process->threads == threads_alive)
        exit(code);
    guest_process->exiting = 1;
    guest_process->status = code;
    interrupt_futexes(guest_process, 0);
    thread_exit(code);
}

_Noreturn void thread_exit(int code) {
    account_cpu();
    struct linux_thread *self = current;
    struct linux_thread **link = &active_threads;
    while (*link != self)
        link = &(*link)->next;
    *link = self->next;
    if (self->clear_tid && !(self->clear_tid % 4)) {
        int *word = guest_ptr(self->clear_tid, 4, PROT_WRITE);
        if (word) {
            __atomic_store_n(word, 0, __ATOMIC_RELEASE);
            thread_futex_wake(word, 1);
        }
    }
    if (!--guest_process->threads) {
        if (!guest_process->exiting)
            guest_process->status = code;
        for (struct guest_process *p = processes, *next; p; p = next) {
            next = p->next;
            if (process_pid() == 1 && p != guest_process && p->threads &&
                namespace_pid(&guest_process->pids, &p->pids, p->pid)) {
                p->exiting = 1;
                p->term_signal = SIGKILL;
                p->status = 128 + SIGKILL;
                interrupt_futexes(p, 0);
            }
            if (p->ppid == guest_process->pid) {
                p->ppid = 0;
                if (p->pids.depth && p->pids.init[p->pids.depth - 1] != guest_process->pid &&
                    p->pids.init[p->pids.depth - 1] != p->pid)
                    p->ppid = p->pids.init[p->pids.depth - 1];
                if (!p->ppid && !p->threads)
                    reap(p);
            }
        }
        for (struct guest_process *parent = processes; parent; parent = parent->next) {
            if (!parent->threads || parent->pid != guest_process->ppid)
                continue;
            struct linux_sigaction *action = child_action(parent);
            if (action->handler > 1 && !parent->child_signal_pid) {
                parent->child_signal_pid =
                    namespace_pid(&parent->pids, &guest_process->pids, guest_process->pid);
                parent->child_signal_code = guest_process->term_signal ? CLD_KILLED : CLD_EXITED;
                parent->child_signal_status =
                    guest_process->term_signal ? guest_process->term_signal : guest_process->status;
                interrupt_futexes(parent, SIGCHLD);
            }
            if (action->handler == 1 || (action->flags & SA_NOCLDWAIT))
                guest_process->ppid = 0;
            break;
        }
        if (guest_process != &root_process && !guest_process->ppid) {
            release_process(guest_process);
            reap(guest_process);
        } else {
            release_process(guest_process);
        }
    }
    if (!--threads_alive)
        exit(root_process.status);
    if (self != &leader)
        free(self);
    current = NULL;
    thread_pause();
    pthread_exit(NULL);
}

struct guest_signal_frame {
    siginfo_t info;
    ucontext_t context;
};
_Static_assert(sizeof(siginfo_t) == 128 && offsetof(ucontext_t, uc_mcontext) == 176,
               "AArch64 Linux signal frame layout");

void signal_trampoline(void) {
    u64 va = map_linux(0, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, NULL, 0);
    const unsigned code[] = {0xd2801168, 0xd4000001}; // mov x8, #139 (rt_sigreturn); svc #0
    void *out = guest_ptr(va, sizeof code, PROT_WRITE);
    if (!out)
        fail("cannot allocate guest signal trampoline");
    memcpy(out, code, sizeof code);
    if (page_update(va, va + PAGE, PROT_READ | PROT_EXEC, 1))
        fail("cannot protect guest signal trampoline");
    guest_process->sigreturn_pc = va;
}

static const u64 chld_bit = 1ULL << (SIGCHLD - 1);
int signal_supported(int sig) {
    switch (sig) {
    case SIGHUP:
    case SIGINT:
    case SIGQUIT:
    case SIGTERM:
    case SIGPIPE:
    case SIGALRM:
    case SIGUSR1:
    case SIGUSR2:
    case SIGABRT:
    case SIGCHLD:
        return 1;
    default:
        return 0;
    }
}
int signal_ready(void) {
    if (guest_process->child_signal_pid && !(current->mask & chld_bit) &&
        child_action(guest_process)->handler > 1)
        return SIGCHLD;
    u64 available = guest_process->pending_signals & ~current->mask;
    return available ? __builtin_ctzll(available) + 1 : 0;
}
long signal_process(int pid, int sig) {
    if (pid <= 0)
        return -ENOTSUP;
    if ((unsigned)sig >= 65)
        return -EINVAL;
    for (struct guest_process *p = processes; p; p = p->next) {
        if (namespace_pid(&guest_process->pids, &p->pids, p->pid) != pid)
            continue;
        if (!sig || !p->threads)
            return 0;
        if (!signal_supported(sig) && sig != SIGKILL)
            return -ENOTSUP;
        struct linux_sigaction *action = process_action(p, sig);
        int sender = namespace_pid(&p->pids, &guest_process->pids, guest_process->pid);
        if (p->pids.depth && p->pids.init[p->pids.depth - 1] == p->pid && action->handler <= 1 &&
            (sig != SIGKILL || sender))
            return 0;
        if (action->handler == 1 || (!action->handler && sig == SIGCHLD))
            return 0;
        if (sig == SIGKILL) {
            p->term_signal = sig;
            p->exiting = 1;
            p->status = 128 + sig;
            interrupt_futexes(p, 0);
            if (p == guest_process)
                process_exit(p->status);
        } else {
            u64 bit = 1ULL << (sig - 1);
            if (!(p->pending_signals & bit))
                p->senders[sig] = sender;
            p->pending_signals |= bit;
            interrupt_futexes(p, sig);
        }
        return 0;
    }
    return -ESRCH;
}
long masked_poll(CPU *cpu) {
    int epoll = cpu->x[8] == SYS_epoll_pwait;
    u64 address = cpu->x[epoll ? 4 : 3];
    if (!address)
        return event_service(cpu);
    u64 previous = current->mask;
    int rc = thread_mask(SIG_SETMASK, address, 0, cpu->x[epoll ? 5 : 4]);
    if (rc)
        return rc;
    long result = event_service(cpu);
    if (result == -EINTR && signal_ready()) {
        current->saved_poll_mask = previous;
        current->restore_poll_mask = 1;
    } else {
        current->mask = previous;
    }
    return result;
}
void signal_checkpoint(CPU *cpu) {
    int sig = signal_ready();
    if (!sig)
        return;
    struct linux_sigaction *action = process_action(guest_process, sig);
    if (action->handler <= 1) {
        guest_process->pending_signals &= ~(1ULL << (sig - 1));
        if (action->handler == 1 || sig == SIGCHLD)
            return;
        guest_process->term_signal = sig;
        process_exit(128 + sig);
    }
    u64 sp = *cpu_cur_sp(cpu);
    u64 va = (sp - sizeof(struct guest_signal_frame)) & ~(u64)15;
    struct guest_signal_frame *frame = guest_ptr(va, sizeof *frame, PROT_WRITE);
    if (!frame)
        fail("invalid guest signal stack");
    memset(frame, 0, sizeof *frame);
    frame->info.si_signo = sig;
    frame->info.si_uid = namespace_id(0);
    if (sig == SIGCHLD && guest_process->child_signal_pid) {
        frame->info.si_code = guest_process->child_signal_code;
        frame->info.si_pid = guest_process->child_signal_pid;
        frame->info.si_status = guest_process->child_signal_status;
        guest_process->child_signal_pid = 0;
    } else {
        frame->info.si_code = SI_USER;
        frame->info.si_pid = guest_process->senders[sig];
        guest_process->pending_signals &= ~(1ULL << (sig - 1));
    }
    frame->context.uc_stack.ss_flags = SS_DISABLE;
    u64 old = current->restore_poll_mask ? current->saved_poll_mask : current->mask;
    current->restore_poll_mask = 0;
    memcpy(&frame->context.uc_sigmask, &old, sizeof old);
    mcontext_t *sc = &frame->context.uc_mcontext;
    memcpy(sc->regs, cpu->x, sizeof sc->regs);
    sc->sp = sp;
    sc->pc = cpu->pc;
    sc->pstate = cpu->nzcv;
    struct fpsimd_context *fp = (void *)sc->__reserved;
    fp->head = (struct _aarch64_ctx){FPSIMD_MAGIC, sizeof *fp};
    fp->fpsr = cpu->fpsr;
    fp->fpcr = cpu->fpcr;
    memcpy(fp->vregs, cpu->v, sizeof fp->vregs);
    current->mask |= action->mask | ((action->flags & SA_NODEFER) ? 0 : (1ULL << (sig - 1)));
    current->mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
    *cpu_cur_sp(cpu) = va;
    cpu->x[30] = action->flags & SA_RESTORER ? action->restorer : guest_process->sigreturn_pc;
    cpu->x[0] = sig;
    cpu->x[1] = va;
    cpu->x[2] = va + offsetof(struct guest_signal_frame, context);
    cpu->pc = action->handler;
    cpu->excl_valid = false;
    if (action->flags & SA_RESETHAND)
        action->handler = 0;
}
long signal_return(CPU *cpu) {
    u64 sp = *cpu_cur_sp(cpu);
    const struct guest_signal_frame *frame = guest_ptr(sp, sizeof *frame, PROT_READ);
    if (!frame || sp % 16)
        fail("invalid guest rt_sigreturn frame");
    const mcontext_t *sc = &frame->context.uc_mcontext;
    const struct fpsimd_context *fp = (const void *)sc->__reserved;
    if (sc->pc % 4 || !guest_ptr(sc->pc, 4, PROT_EXEC) || fp->head.magic != FPSIMD_MAGIC ||
        fp->head.size != sizeof *fp)
        fail("invalid guest signal context");
    memcpy(&current->mask, &frame->context.uc_sigmask, sizeof current->mask);
    current->mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
    memcpy(cpu->x, sc->regs, sizeof sc->regs);
    memcpy(cpu->v, fp->vregs, sizeof fp->vregs);
    cpu->fpcr = fp->fpcr;
    cpu->fpsr = fp->fpsr;
    cpu->nzcv = sc->pstate & 0xf0000000;
    cpu->pc = sc->pc;
    *cpu_cur_sp(cpu) = sc->sp;
    cpu->excl_valid = false;
    return cpu->x[0];
}
