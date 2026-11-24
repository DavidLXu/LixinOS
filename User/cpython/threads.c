// Single-vCPU pthread backend. Each runnable thread has a musl TLS block and
// its own stack; the interpreter checkpoints and blocking syscalls yield.
#include "../console.h"
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>

struct context {
    uintptr_t saved[14];
    double fp[8];
};
struct thread {
    struct context registers;
    int id, *clear_tid, *wait_word;
    uint64_t deadline, signal_mask;
    int (*entry)(void *);
    void *arg;
    char name[16];
};
static struct thread threads[64] = {{.id = 1}};
static int current, next_id = 2;
static unsigned active = 1;
extern void lx_switch(struct context *, const struct context *);
extern long lx_munmap(uintptr_t, unsigned long);
static uint64_t micros(void) { return console_call(38, 1); }

int lx_thread_id(void) { return threads[current].id; }
int lx_thread_main(void) { return current == 0; }
int lx_threads_active(void) { return active > 1; }
long lx_prctl(int option, uintptr_t address) {
    struct thread *thread = &threads[current];
    if (option != PR_SET_NAME && option != PR_GET_NAME)
        return -ENOSYS;
    if (!address)
        return -EFAULT;
    if (option == PR_GET_NAME)
        memcpy((void *)address, thread->name, sizeof(thread->name));
    else
        strncpy(thread->name, (const char *)address, sizeof(thread->name) - 1);
    return 0;
}
long lx_clear_tid(int *word) {
    threads[current].clear_tid = word;
    return lx_thread_id();
}

void lx_schedule(void) {
    for (;;) {
        uint64_t now = micros(), earliest = UINT64_MAX;
        for (unsigned n = 1; n <= 64; ++n) {
            int i = (current + n) % 64;
            struct thread *t = &threads[i];
            if (!t->id)
                continue;
            if (t->wait_word || t->deadline) {
                if (!t->deadline || now < t->deadline) {
                    if (t->deadline && t->deadline < earliest)
                        earliest = t->deadline;
                    continue;
                }
                t->wait_word = NULL;
            }
            if (i != current) {
                int old = current;
                current = i;
                lx_switch(&threads[old].registers, &t->registers);
            }
            return;
        }
        unsigned long delay = earliest == UINT64_MAX ? 10 : (earliest - now + 999) / 1000;
        console_call(43, delay > 10 ? 10 : delay);
    }
}
static int wake(int *word, int count) {
    int done = 0;
    for (unsigned i = 0; i < 64 && done < count; i++)
        if (threads[i].id && threads[i].wait_word == word) {
            threads[i].wait_word = NULL;
            threads[i].deadline = 0;
            done++;
        }
    return done;
}
__attribute__((noreturn)) void lx_thread_exit(void *unmap, size_t length) {
    struct thread *t = &threads[current];
    if (t->clear_tid) {
        *t->clear_tid = 0;
        wake(t->clear_tid, 1);
    }
    if (unmap)
        lx_munmap((uintptr_t)unmap, length); // bookkeeping only, switched before reuse
    t->id = 0;
    if (!--active)
        finish(0);
    lx_schedule();
    __builtin_unreachable();
}
static void start(void) {
    struct thread *t = &threads[current];
    t->entry(t->arg);
    lx_thread_exit(NULL, 0);
}
long lx_clone(int (*fn)(void *), void *sp, int flags, void *arg, int *ptid, void *tls, int *ctid) {
    (void)flags;
    for (unsigned i = 1; i < 64; i++)
        if (!threads[i].id) {
            struct thread *t = &threads[i];
            memset(t, 0, sizeof(*t));
            t->registers.saved[11] = (uintptr_t)start; // x30
            t->registers.saved[12] = (uintptr_t)sp & ~(uintptr_t)15;
            t->registers.saved[13] = (uintptr_t)tls;
            t->entry = fn;
            t->arg = arg;
            t->clear_tid = ctid;
            t->signal_mask = threads[current].signal_mask;
            memcpy(t->name, threads[current].name, sizeof(t->name));
            *ptid = t->id = next_id++;
            ++active;
            return t->id;
        }
    return -EAGAIN;
}
long lx_sigmask(int how, const uint64_t *set, uint64_t *old) {
    uint64_t *mask = &threads[current].signal_mask, previous = *mask;
    if (set) {
        if (how == 0)
            *mask |= *set;
        else if (how == 1)
            *mask &= ~*set;
        else if (how == 2)
            *mask = *set;
        else
            return -EINVAL;
    }
    if (old)
        *old = previous;
    return 0;
}
long lx_futex(int *word, int op, int val, const struct timespec *ts, int *target, int expected) {
    int cmd = op & 127;
    if (cmd == 1)
        return wake(word, val);
    if (cmd == 3 || cmd == 4) { // FUTEX_REQUEUE and FUTEX_CMP_REQUEUE
        int count = (int)(uintptr_t)ts;
        if (val < 0 || count < 0 || !target || word == target) return -EINVAL;
        if (cmd == 4 && *word != expected) return -EAGAIN;
        int woken = wake(word, val);
        int moved = 0;
        // musl hands condition-variable waiters to the associated mutex.
        for (unsigned i = 0; i < 64 && moved < count; ++i) {
            if (threads[i].id && threads[i].wait_word == word) {
                threads[i].wait_word = target;
                ++moved;
            }
        }
        return woken + moved;
    }
    if (cmd != 0 && cmd != 9)
        return -ENOSYS;
    if (*word != val)
        return -EAGAIN;
    struct thread *t = &threads[current];
    uint64_t deadline = 0;
    if (ts) {
        if (ts->tv_sec < 0 || (unsigned long)ts->tv_nsec >= 1000000000)
            return -EINVAL;
        uint64_t now = micros(), us = (uint64_t)ts->tv_sec * 1000000 + ts->tv_nsec / 1000;
        if (cmd == 0)
            deadline = now + us;
        else if (op & 256) {
            uint64_t wall = console_call(36, 0) * 1000000UL;
            deadline = now + (us > wall ? us - wall : 0);
        } else
            deadline = us;
        if (deadline <= now)
            return -ETIMEDOUT;
    }
    t->wait_word = word;
    t->deadline = deadline;
    lx_schedule();
    int timed_out = t->deadline != 0;
    t->deadline = 0;
    return timed_out ? -ETIMEDOUT : 0;
}
void lx_delay(unsigned ms) {
    if (active == 1) {
        console_call(43, ms);
        return;
    }
    threads[current].deadline = micros() + (unsigned long)ms * 1000;
    lx_schedule();
    threads[current].deadline = 0;
}
