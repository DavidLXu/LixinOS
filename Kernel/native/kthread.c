#include "kthread.h"
#include "smp.h"
#include "vm.h"

typedef unsigned long word;
enum { SLOTS = KT_TASK_SLOTS };
extern unsigned kt_slot(void);
extern long kt_id(unsigned slot);
extern void kt_park(word ticks), kt_unpark(unsigned slot);
extern long kt_new(void);
extern void kt_stop(unsigned slot);
extern int kt_stopped(void);
extern void kt_sleep(unsigned ms);
extern word smp_ticks(void), smp_frequency(void);
extern long enter_thread(word entry, word sp, word tls, word arg, word fpcr, word fpsr);
extern word kt_fpcr(void), kt_fpsr(void);
extern int kt_access(word addr, int write);
struct thread {
    unsigned owner_plus_one;
    word pc, sp, tls, arg, clear, waiting, fpcr, fpsr;
    unsigned local_index;
};
static struct thread threads[SLOTS];
static unsigned members[SLOTS], service[SLOTS], depth[SLOTS];
static int closing[SLOTS];
static long group_result[SLOTS];

unsigned kt_worker(unsigned slot) { return slot < SLOTS && threads[slot].owner_plus_one; }
unsigned kt_owner(unsigned slot) {
    return kt_worker(slot) ? threads[slot].owner_plus_one-1 : slot;
}
static int user_access(word addr,int write) {
    return kt_access(addr,write) || (vm_touch(addr,write,0) && kt_access(addr,write));
}
static int accessible(word addr, word bytes, int write) {
    return addr && addr + bytes > addr && user_access(addr,write) && user_access(addr+bytes-1,write);
}
static int valid_int(word addr, int write) {
    return !(addr & 3) && accessible(addr,4,write);
}
static unsigned wake(unsigned owner, word addr, unsigned count, word target) {
    unsigned n=0;
    for (unsigned i=0; i<SLOTS && n<count; ++i)
        if (kt_owner(i)==owner && threads[i].waiting==addr) {
            threads[i].waiting=target;
            if (!target) kt_unpark(i);
            ++n;
        }
    return n;
}
static void clear_tid(unsigned owner, struct thread *t) {
    if (valid_int(t->clear,1)) {
        __asm__ volatile("dmb ish" ::: "memory");
        *(volatile int *)t->clear=0;
        wake(owner,t->clear,~0u,0);
    }
    t->clear=0;
}
static void stop_group(unsigned owner, long result) {
    if (!closing[owner]) { closing[owner]=1; group_result[owner]=result; }
    for (unsigned i=0;i<SLOTS;i++)
        if (kt_worker(i) && kt_owner(i)==owner && i!=kt_slot()) kt_stop(i);
}
long kt_run(unsigned slot) {
    struct thread *t=&threads[slot];
    unsigned owner=kt_owner(slot);
    long status=kt_stopped() ? KT_THREAD_DONE : enter_thread(t->pc,t->sp,t->tls,t->arg,t->fpcr,t->fpsr);
    if (status!=KT_THREAD_DONE && !closing[owner]) {
        stop_group(owner,status);
        kt_stop(owner);
    }
    clear_tid(owner,t);
    *t=(struct thread){0};
    --members[owner];
    return 0;
}
/* Called before the leader can unmap or replace any part of the image. */
long kt_end_group(long status) {
    unsigned slot=kt_slot();
    if (members[slot]) {
        stop_group(slot,status);
        while (members[slot]) kt_sleep(1);
    }
    if (closing[slot]) status=group_result[slot];
    closing[slot]=0;
    threads[slot]=(struct thread){0};
    return status;
}
/* Services using the legacy process mailbox and file context can suspend in
 * the kernel. Keep another thread out of that context across such waits. */
void kt_service_enter(void) {
    unsigned slot=kt_slot(),owner=kt_owner(slot);
    while (service[owner] && service[owner]!=slot+1) kt_sleep(1);
    service[owner]=slot+1;
    ++depth[owner];
}
void kt_service_leave(void) {
    unsigned owner=kt_owner(kt_slot());
    if (!--depth[owner]) service[owner]=0;
}

long kt_call(word address) {
    unsigned slot=kt_slot(),owner=kt_owner(slot);
    struct thread *self=&threads[slot];
    if (!address) return 1; /* ABI version */
    if ((address & 7) || !accessible(address,8*sizeof(word),0)) return -14;
    const volatile word *r=(const volatile word *)address;
    word op=r[0], a=r[1], b=r[2], c=r[3], d=r[4], e=r[5], f=r[6];
    switch(op) {
    case KT_SELF: return kt_id(slot);
    case KT_INDEX: return self->local_index;
    case KT_WORKER: return !!kt_worker(slot);
    case KT_COUNT: return members[owner]+1;
    case KT_CLEAR_TID:
        if (a && !valid_int(a,1)) return -14;
        self->clear=a; return kt_id(slot);
    case KT_CREATE: {
        if (closing[owner] || kt_stopped()) return -11;
        if ((a&3) || !accessible(a,4,0) || (b&15) || b<16 ||
            !accessible(b-16,16,1) || !accessible(c,1,0) || !valid_int(e,1) || !valid_int(f,1)) return -14;
        /* Existing musl runtimes have sixteen per-process thread exit stacks. */
        unsigned limit=r[7]==KT_PROCESS_THREADS ? KT_PROCESS_THREADS : KT_LEGACY_THREADS;
        word used=1;
        for (unsigned i=0;i<SLOTS;i++)
            if (kt_worker(i) && kt_owner(i)==owner) used |= 1UL<<threads[i].local_index;
        unsigned index=1;
        while (index<limit && (used & (1UL<<index))) ++index;
        if (index==limit) return -11;
        long child=kt_new();
        if (child<0) return -11;
        threads[child]=(struct thread){owner+1,a,b,c,d,e,0,kt_fpcr(),kt_fpsr(),index};
        *(volatile int *)f=(int)kt_id(child);
        ++members[owner];
        return kt_id(child);
    }
    case KT_YIELD: kt_sleep(0); return 0;
    case KT_WAIT: {
        if (!valid_int(a,0)) return -14;
        if (*(volatile int *)a!=(int)b) return -11;
        self->waiting=a;
        /* c is an absolute monotonic deadline in microseconds, or zero. */
        word freq=smp_frequency();
        while (self->waiting && !kt_stopped()) {
            word ticks=smp_ticks();
            if (c && (ticks/freq)*1000000+(ticks%freq)*1000000/freq>=c) {
                self->waiting=0; return -110;
            }
            word deadline=~0UL;
            if (c && c/1000000 < (~0UL/freq)-1)
                deadline=(c/1000000)*freq+((c%1000000)*freq+999999)/1000000;
            kt_park(deadline);
        }
        self->waiting=0; return kt_stopped() ? -4 : 0;
    }
    case KT_WAKE:
    case KT_REQUEUE: {
        if (!valid_int(a,0) || (int)b<0) return -22;
        if (op==KT_REQUEUE && (!valid_int(c,0) || c==a || (int)d<0)) return -22;
        if (op==KT_REQUEUE && e>>32 && *(volatile int *)a!=(int)e) return -11;
        unsigned n=wake(owner,a,b,0);
        return n+(op==KT_REQUEUE ? wake(owner,a,d,c) : 0);
    }
    case KT_LEADER_EXIT:
        /* Main pthread_exit keeps the process alive for the other threads. */
        if (!kt_worker(slot)) {
            clear_tid(owner,self);
            while (members[owner] && !kt_stopped()) kt_sleep(1);
        }
        return 0;
    }
    return -38;
}
