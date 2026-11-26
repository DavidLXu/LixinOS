// Native threads use scheduler slots. The cooperative backend serves older kernels.
#include "../../Kernel/native/kthread.h"
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
/* The cooperative exit path switches directly from the departing stack.
 * Reclaim it after a surviving thread has resumed on its own stack. */
static void *retired_stack;
static size_t retired_length;
static struct thread threads[64] = {{.id = 1}};
static int current, next_id = 2;
static unsigned active = 1;
extern void lx_switch(struct context *, const struct context *);
extern long lx_munmap(uintptr_t, unsigned long);
static uint64_t micros(void) { return console_call(38, 1); }

static int capability, native_started, gate_owner;
static unsigned gate_depth;
static int native(void) {
#ifdef LX_PTHREAD_COOPERATIVE_ONLY
    return 0;
#else
    int mode = __atomic_load_n(&capability, __ATOMIC_RELAXED);
    if (!mode) {
        mode = console_call(61, 0) == 1 ? 1 : -1;
        __atomic_store_n(&capability, mode, __ATOMIC_RELAXED);
    }
    return mode > 0;
#endif
}
static long request(long op, long a, long b, long c, long d, long e, long f) {
    long words[8] = {op, a, b, c, d, e, f, op==KT_CREATE ? KT_PROCESS_THREADS : 0};
    return console_call(61, (long)words);
}
static long simple(long op) { return request(op,0,0,0,0,0,0); }
int lx_thread_id(void) { return native() ? simple(KT_SELF) : threads[current].id; }
int lx_thread_main(void) { return native() ? !simple(KT_WORKER) : current == 0; }
int lx_cooperative_active(void) { return !native() && active>1; }
int lx_threads_active(void) {
    return native() ? __atomic_load_n(&native_started,__ATOMIC_RELAXED) && simple(KT_COUNT)>1 : active>1;
}
static unsigned index_self(void) { return native() ? simple(KT_INDEX) : current; }

/* File descriptors, allocator bookkeeping and the SDK mailbox are shared. */
int lx_runtime_lock(void) {
    if (!__atomic_load_n(&native_started,__ATOMIC_ACQUIRE)) return 0;
    int id=lx_thread_id();
    while (__atomic_load_n(&gate_owner,__ATOMIC_RELAXED)!=id) {
        int owner=0;
        if (__atomic_compare_exchange_n(&gate_owner,&owner,id,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) break;
        request(KT_WAIT,(long)&gate_owner,owner,0,0,0,0);
    }
    ++gate_depth;
    return 1;
}
void lx_runtime_unlock(int *locked) {
    if (*locked && !--gate_depth) {
        __atomic_store_n(&gate_owner,0,__ATOMIC_RELEASE);
        request(KT_WAKE,(long)&gate_owner,1,0,0,0,0);
    }
}
static unsigned drop_gate(void) {
    int owner=__atomic_load_n(&gate_owner,__ATOMIC_RELAXED);
    if (!owner || owner!=lx_thread_id()) return 0;
    unsigned depth=gate_depth;
    gate_depth=1;
    int held=1;
    lx_runtime_unlock(&held);
    return depth;
}
static void restore_gate(unsigned depth) {
    if (depth) { lx_runtime_lock(); gate_depth=depth; }
}
long lx_prctl(int option, uintptr_t address) {
    struct thread *thread = &threads[index_self()];
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
    if (native()) return request(KT_CLEAR_TID,(long)word,0,0,0,0,0);
    threads[current].clear_tid = word;
    return lx_thread_id();
}

static void reap_stack(void) {
    if (retired_stack) {
        void *stack = retired_stack;
        retired_stack = NULL;
        lx_munmap((uintptr_t)stack, retired_length);
    }
}
void lx_schedule(void) {
    if (native()) {
        unsigned held=drop_gate();
        simple(KT_YIELD);
        restore_gate(held);
        return;
    }
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
                reap_stack();
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
static unsigned char exit_stacks[KT_PROCESS_THREADS][4096] __attribute__((aligned(16)));
extern _Noreturn void lx_exit_on_stack(void *, size_t, void *, int);
_Noreturn void lx_finish_native_thread(void *unmap, size_t length, void *unused, int worker) {
    if (unmap) {
        int held=lx_runtime_lock();
        lx_munmap((uintptr_t)unmap,length);
        lx_runtime_unlock(&held);
    }
    if (!worker) simple(KT_LEADER_EXIT);
    finish(worker ? KT_THREAD_DONE : 0);
}
__attribute__((noreturn)) void lx_thread_exit(void *unmap, size_t length) {
    if (native()) {
        int worker=simple(KT_WORKER);
        unsigned idx=index_self();
        lx_exit_on_stack(unmap,length,&exit_stacks[idx][sizeof(exit_stacks[idx])],worker);
    }
    struct thread *t = &threads[current];
    if (t->clear_tid) {
        *t->clear_tid = 0;
        wake(t->clear_tid, 1);
    }
    retired_stack = unmap;
    retired_length = length;
    t->id = 0;
    if (!--active)
        finish(0);
    lx_schedule();
    __builtin_unreachable();
}
static void start(void) {
    reap_stack();
    struct thread *t = &threads[current];
    t->entry(t->arg);
    lx_thread_exit(NULL, 0);
}
struct native_start {
    int (*entry)(void *);
    void *arg;
    uint64_t mask;
    char name[16];
};
static _Noreturn void native_entry(struct native_start *start) {
    struct thread *self=&threads[index_self()];
    self->signal_mask=start->mask;
    memcpy(self->name,start->name,sizeof(self->name));
    start->entry(start->arg);
    lx_thread_exit(NULL,0);
}
long lx_clone(int (*fn)(void *), void *sp, int flags, void *arg, int *ptid, void *tls, int *ctid) {
    (void)flags;
    if (native()) {
        struct native_start *start=(void *)(((uintptr_t)sp-sizeof(*start)) & ~(uintptr_t)15);
        struct thread *parent=&threads[index_self()];
        start->entry=fn;
        start->arg=arg;
        start->mask=parent->signal_mask;
        memcpy(start->name,parent->name,sizeof(start->name));
        __atomic_store_n(&native_started,1,__ATOMIC_RELEASE);
        return request(KT_CREATE,(long)native_entry,(long)start,(long)tls,
                       (long)start,(long)ctid,(long)ptid);
    }
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
    uint64_t *mask = &threads[index_self()].signal_mask, previous = *mask;
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
    if (native() && (cmd==1 || cmd==3 || cmd==4)) {
        long compare=cmd==4 ? (1UL<<32)|(unsigned)expected : 0;
        return request(cmd==1 ? KT_WAKE : KT_REQUEUE,(long)word,val,(long)target,(long)ts,compare,0);
    }
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
    if (native()) return request(KT_WAIT,(long)word,val,deadline,0,0,0);
    t->wait_word = word;
    t->deadline = deadline;
    lx_schedule();
    int timed_out = t->deadline != 0;
    t->deadline = 0;
    return timed_out ? -ETIMEDOUT : 0;
}
void lx_delay(unsigned ms) {
    if (native()) {
        unsigned held=drop_gate();
        console_call(43,ms);
        restore_gate(held);
        return;
    }
    if (active == 1) {
        console_call(43, ms);
        return;
    }
    threads[current].deadline = micros() + (unsigned long)ms * 1000;
    lx_schedule();
    threads[current].deadline = 0;
}

/* The bare-metal GCC emits emulated TLS descriptors for C/C++ thread_local.
 * Keep their aligned values in musl TSD so detached threads release them too. */
#include <pthread.h>
#include <stdlib.h>
struct emutls_descriptor { size_t size, alignment; void *index, *initial; };
struct emutls_value {
    struct emutls_value *next;
    struct emutls_descriptor *descriptor;
    void *value;
};
static pthread_key_t emutls_key;
static pthread_once_t emutls_once=PTHREAD_ONCE_INIT;
static void emutls_free(void *list) {
    struct emutls_value *entry=list;
    while (entry) {
        struct emutls_value *next=entry->next;
        free(entry->value);
        free(entry);
        entry=next;
    }
}
static void emutls_init(void) {
    if (pthread_key_create(&emutls_key,emutls_free)) abort();
}
void *__emutls_get_address(void *object) {
    struct emutls_descriptor *descriptor=object;
    pthread_once(&emutls_once,emutls_init);
    struct emutls_value *head=pthread_getspecific(emutls_key);
    for (struct emutls_value *e=head;e;e=e->next)
        if (e->descriptor==descriptor) return e->value;
    struct emutls_value *e=malloc(sizeof(*e));
    size_t alignment=descriptor->alignment;
    if (alignment<sizeof(void *)) alignment=sizeof(void *);
    if (!e || posix_memalign(&e->value,alignment,descriptor->size)) abort();
    if (descriptor->initial) memcpy(e->value,descriptor->initial,descriptor->size);
    else memset(e->value,0,descriptor->size);
    e->descriptor=descriptor;
    e->next=head;
    if (pthread_setspecific(emutls_key,e)) abort();
    return e->value;
}
