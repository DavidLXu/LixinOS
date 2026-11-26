#include "native.h"
#include "identity.h"
#include "smp.h"
#include "kthread.h"
extern void smp_native_root(void);

enum { FREE, READY, WAITING, EXITED, PAUSED, CHANNEL, FUTEX, IDLE };
struct task {
    u64 stack, pid, parent, waiting, cpu, console_deadline;
    int state, detached, job, watching, stop;
    unsigned terminal, core;
    int thread;
    void *channel;
    long status;
};
static struct task tasks[KT_TASK_SLOTS + 2];
static u8 stacks[KT_TASK_SLOTS + 2][64 * 1024] __attribute__((aligned(16)));
static unsigned running[2] = {0, KT_TASK_SLOTS+1}, gic3;
#define current running[smp_cpu()]
static u64 next_pid = 2, irqs;
u64 timer_count(void) { return irqs; }
long task_list(char *out) {
    char *p = add_text(out, "PID PPID STATE MODE\n");
    for (unsigned i = 0; i < KT_TASK_SLOTS; i++) {
        const struct task *t = &tasks[i];
        if (!t->state)
            continue;
        p = add_number(p, t->pid);
        *p++ = ' ';
        p = add_number(p, t->parent);
        p = add_text(p, (t->state == FUTEX || t->state == WAITING || t->state == CHANNEL) ? " waiting "
                        : t->state == EXITED ? " exited "
                        : i == running[t->core] ? " running "
                                             : " ready ");
        p = add_text(p, t->detached ? "background\n" : "foreground\n");
    }
    return p - out;
}
extern void swap_task(u64 *old, u64 resume, u64 fresh_top);
extern void arm_timer(u64 ticks), gic3_enable(void), gic3_eoi(u64 id);
extern u64 gic3_ack(void);
#define MMIO32(p) (*(volatile u32 *)(u64)(p))

static u64 busy_starts[2];
#define busy_start busy_starts[smp_cpu()]
void tasks_init(void) {
    busy_start = clock_ticks();
    tasks[0].pid = 1;
    tasks[0].state = READY;
    gic3 = ((MMIO32(0x0800ffe8) >> 4) & 15) >= 3;
    if (gic3) {
        MMIO32(0x080a0014) &= ~2u;
        while (MMIO32(0x080a0014) & 4) {
        }
        MMIO32(0x080b0080) |= 1u << 27;
        *(volatile u8 *)0x080b041b = 128;
        MMIO32(0x080b0100) = 1u << 27;
        MMIO32(0x08000000) |= 3;
        asm volatile("dsb sy" ::: "memory");
        gic3_enable();
    } else {
        MMIO32(0x08000100) = 1u << 27;
        *(volatile u8 *)0x0800041b = 128;
        MMIO32(0x08000000) = 1;
        MMIO32(0x08010004) = 255;
        MMIO32(0x08010000) = 1;
    }
    tasks[KT_TASK_SLOTS].state = tasks[KT_TASK_SLOTS+1].state = IDLE;
    tasks[KT_TASK_SLOTS+1].core = 1;
    smp_init(gic3);
    arm_timer(clock_hz() / 50);
}
static u64 sample_at, sample_idle, sample_remote;
static unsigned busy;
u64 idle_ticks(void) { return smp_idle_ticks(0); }
unsigned cpu_percent(void) {
    u64 now = clock_ticks(), delta = now - sample_at;
    if (delta >= clock_hz() / 2) {
        u64 idle = idle_ticks(), waited = idle - sample_idle;
        u64 remote = smp_busy_ticks(), work = remote - sample_remote;
        if (work > delta) work = delta;
        busy = ((waited < delta ? delta - waited : 0) + work) * 100 / (delta * smp_cpus());
        sample_remote = remote;
        sample_at = now;
        sample_idle = idle;
    }
    return busy;
}
/* Foreground waiters only run on child completion or to check for Ctrl+C.
 * Keep WAITING until dispatch so pause/exit can still find the parent. */
static int runnable(unsigned slot, unsigned cpu, u64 now) {
    const struct task *t = &tasks[slot];
    return t->core == cpu && (t->state == READY ||
           ((t->state == WAITING || t->state == FUTEX) && t->console_deadline && now >= t->console_deadline));
}
void idle_slice(void) {
    if (current < KT_TASK_SLOTS) display_status();
    unsigned cpu = smp_cpu();
    u64 now = clock_ticks();
    for (unsigned i=0; i<KT_TASK_SLOTS; i++)
        if (i!=current && runnable(i, cpu, now)) { schedule(); return; }
    smp_idle_wait(clock_hz()/4000);
}
static unsigned choose_core(void) {
    if (smp_cpus()==1) return 0;
    unsigned load[2]={0,0}, children[2]={0,0};
    for (unsigned i=0; i<KT_TASK_SLOTS; i++) {
        struct task *t=&tasks[i];
        if (t->state==FREE || t->state==EXITED) continue;
        if (t->parent==task_pid()) children[t->core]++;
        if (t->state==READY) load[t->core]++;
    }
    if (children[0]!=children[1]) return children[1]<children[0];
    return load[0]==load[1] ? 1-smp_cpu() : load[1]<load[0];
}
static int free_slot(unsigned first, unsigned end) {
    for (unsigned i = first; i < end; i++)
        if (tasks[i].state == FREE)
            return i;
    return -1;
}
int task_slot(void) { return free_slot(1, NATIVE_TASKS); }
long task_create(unsigned slot, int detached) {
    if (!next_pid || tasks[slot].state != FREE)
        return -1;
    id_inherit(slot,task_active());
    tasks[slot] = (struct task){
        .core = choose_core(), .pid = next_pid++, .parent = task_pid(), .state = READY, .detached = detached,
        .terminal = task_terminal() ? task_terminal() : popup_terminal()};

    return tasks[slot].pid;
}
unsigned kt_slot(void) { return current; }
long kt_new(void) {
    int slot=free_slot(NATIVE_TASKS, KT_TASK_SLOTS);
    if (slot<0 || task_create(slot,0)<0) return -1;
    unsigned owner=kt_owner(current), load[2]={0,0};
    load[tasks[owner].core]++;
    for (unsigned i=1;i<KT_TASK_SLOTS;i++)
        if (tasks[i].thread && kt_owner(i)==owner) load[tasks[i].core]++;
    tasks[slot].core=smp_cpus()==2 && load[1]<load[0];
    tasks[slot].thread=1;
    tasks[slot].parent=tasks[owner].pid;
    return slot;
}
long kt_id(unsigned slot) { return tasks[slot].pid; }
void kt_park(u64 deadline) {
    tasks[current].console_deadline=deadline;
    tasks[current].state=FUTEX;
    schedule();
    tasks[current].console_deadline=0;
}
void kt_unpark(unsigned slot) {
    if (tasks[slot].state==FUTEX) tasks[slot].state=READY;
}
int kt_stopped(void) { return task_stopped(); }
void kt_stop(unsigned i) {
    struct task *t=&tasks[i];
    if (running[t->core]==i) smp_revoke_clock(t->core);
    t->stop=137;
    if (t->state==FUTEX || t->state==CHANNEL || t->state==PAUSED || t->state==WAITING) t->state=READY;
}
void kt_sleep(unsigned ms) {
    u64 start=clock_ticks(),delay=clock_hz()/1000*ms;
    do { if (ms) idle_slice(); else schedule(); } while(clock_ticks()-start<delay);
}
unsigned task_active(void) { return kt_owner(current); }
u64 task_pid(void) { return tasks[kt_owner(current)].pid; }
long task_query(u64 value) {
    if (!value) return task_pid();
    if (value==~0UL) return tasks[task_active()].parent;
    for (unsigned i=0;i<KT_TASK_SLOTS;++i)
        if (!tasks[i].thread && tasks[i].state!=FREE && tasks[i].pid==value) return value;
    return 0;
}
unsigned task_terminal(void) { return tasks[current].terminal; }
long task_resources(u64 *out) {
    u64 now=clock_ticks();
    u64 remote = smp_busy_ticks();
    out[0]=now;
    out[1]=smp_cpus() == 1 ? idle_ticks() : (idle_ticks() + now - remote) / 2;
    out[2]=native_ram_bytes();
    unsigned n=3;
    for(unsigned i=0;i<KT_TASK_SLOTS;i++) {
        struct task *t=&tasks[i];
        if(!t->state) continue;
        out[n]=t->pid; out[n+1]=t->parent;
        out[n+2]=t->cpu+(i==running[t->core] ? now-smp_idle_ticks(t->core)-busy_starts[t->core] : 0);
        out[n+3]=process_private_bytes(i);
        out[n+4]=t->state==EXITED?5:t->state==WAITING||t->state==PAUSED||t->state==CHANNEL||t->state==FUTEX?4:(i==running[t->core])?1:2;
        out[n+5]=t->detached;
        const char *path=process_task_name(i), *name=path;
        for(;*path;path++) if(*path=='/') name=path+1;
        char *dst=(char *)(out+n+6);
        memset(dst,0,32);
        for(unsigned j=0;j<31 && name[j];j++) dst[j]=name[j];
        n+=10;
    }
    return n*8;
}
void schedule(void) {
    unsigned cpu=smp_cpu(), old=current, id=KT_TASK_SLOTS+cpu;
    u64 now = clock_ticks();
    for (unsigned k=1; k<=KT_TASK_SLOTS; k++) {
        unsigned candidate=(old+k)%KT_TASK_SLOTS;
        if (candidate!=old && runnable(candidate, cpu, now)) {
            id=candidate;
            break;
        }
    }
    if (id>=KT_TASK_SLOTS && (old==id || tasks[old].state==READY)) {
        if (old==id) smp_idle_wait(clock_hz()/1000);
        else smp_kernel_yield();
        return;
    }
    u64 busy=clock_ticks()-smp_idle_ticks(cpu);
    tasks[old].cpu+=busy-busy_start;
    busy_start=busy;
    if (tasks[id].state == WAITING || tasks[id].state == FUTEX) tasks[id].state = READY;
    current=id;
    select_bank(kt_owner(id));
    swap_task(&tasks[old].stack,tasks[id].stack,(u64)&stacks[id][sizeof(stacks[id])]);
}
void task_park(void *channel) {
    tasks[current].channel=channel;
    tasks[current].state=CHANNEL;
    schedule();
    tasks[current].channel=0;
}
void task_wake(void *channel) {
    for (unsigned i=0; i<KT_TASK_SLOTS; i++)
        if (tasks[i].state==CHANNEL && tasks[i].channel==channel) tasks[i].state=READY;
}
void task_pause(void) {
    for (unsigned i = 0; i < KT_TASK_SLOTS; i++) {
        if (tasks[i].state != WAITING || tasks[i].waiting != task_pid())
            continue;
        tasks[i].state = READY;
        tasks[current].state = PAUSED;
        schedule();
        return;
    }
}
void task_resume(int slot) {
    if (tasks[slot].state == PAUSED)
        tasks[slot].state = READY;
}
void task_job(unsigned slot) {
    tasks[slot].job = 1;
}
void task_own_interrupt_key(void) { tasks[current].job = 0; }
int task_stopped(void) { return tasks[current].stop; }
static void stop_tree(unsigned slot, int status) {
    struct task *t = &tasks[slot];
    if (t->state == FREE || t->state == EXITED) return;
    if (running[t->core]==slot) smp_revoke_clock(t->core);
    t->stop = status;
    if (t->state == FUTEX || t->state == PAUSED || t->state == CHANNEL) t->state = READY;
    for (unsigned i = 1; i < KT_TASK_SLOTS; i++)
        if (tasks[i].parent == t->pid && !tasks[i].detached) stop_tree(i, status);
}
int task_stop_child(u64 pid) {
    for (unsigned i = 1; i < KT_TASK_SLOTS; i++)
        if (tasks[i].state != FREE && !tasks[i].thread && tasks[i].pid == pid && tasks[i].parent == task_pid()
            && !tasks[i].detached) { stop_tree(i,137); return 0; }
    return -1;
}
static void interrupt_jobs(u64 owner) {
    for (unsigned i = 1; i < KT_TASK_SLOTS; i++)
        if (tasks[i].job && !tasks[i].detached && tasks[i].parent == owner) stop_tree(i,130);
}
static int parent_slot(unsigned slot) {
    if (tasks[slot].detached) return -1;
    for (unsigned i = 0; i < KT_TASK_SLOTS; i++)
        if (tasks[i].state != FREE && tasks[i].pid == tasks[slot].parent) return i;
    return -1;
}
int interrupt_reader(void) {
    if (display_editor_active()) return 0;
    if (graphical_shortcuts()) return 0;
    for (int slot = current, p; (p = parent_slot(slot)) >= 0; slot = p) {
        if (tasks[slot].job && tasks[p].watching) { interrupt_jobs(tasks[p].pid); return 1; }
    }
    return 0;
}
static int owns_console(unsigned child) {
    if (!tasks[child].job || display_editor_active() || focused_pane() != process_pane() || tasks[child].terminal != popup_terminal() || graphical_shortcuts()) return 0;
    for (unsigned i = 1; i < KT_TASK_SLOTS; i++) {
        if (!tasks[i].watching) continue;
        for (int p = i; p >= 0; p = parent_slot(p)) if (p == (int)child) return 0;
    }
    return 1;
}
long task_wait_mode(u64 pid, int nohang) {
    for (unsigned i = 1; i < KT_TASK_SLOTS; i++) {
        struct task *child = &tasks[i];
        if (child->state == FREE || child->thread || child->pid != pid || child->parent != task_pid() ||
            child->detached)
            continue;
        if (child->state != EXITED) {
            if (nohang) return -2;
            struct task *parent = &tasks[current];
            parent->waiting = pid;
            while (child->state != EXITED && child->state != PAUSED) {
                parent->watching = child->job && !parent->detached && !parent->stop;
                if (parent->watching && owns_console(i) && console_interrupt())
                    interrupt_jobs(parent->pid);
                parent->console_deadline = parent->watching ? clock_ticks() + clock_hz() / 100 : 0;
                parent->state = WAITING;
                schedule();
            }
            parent->watching = 0;
            parent->console_deadline = 0;
            parent->waiting = 0;
        }
        if (child->state == PAUSED)
            return -2;
        if (child->state != EXITED)
            return -1;
        child->state = FREE;
        return child->status;
    }
    return -1;
}
long task_wait(u64 pid) { return task_wait_mode(pid, 0); }
void smp_secondary_schedule(void) {
    smp_native_root();
    busy_start=clock_ticks();
    for (;;) schedule();
}
void task_start(void) {
    if (current>=KT_TASK_SLOTS) for (;;) schedule();
    struct task *t = &tasks[current];
    if (t->thread) {
        kt_run(current);
        t->state=FREE;
        t->thread=0;
        schedule();
        for (;;) {}
    }
    long status = run_task(current);
    /* A child that could not execute still has an exit status for waitpid. */
    t->status = status < 0 ? 126 : status;
    for (unsigned i = 0; i < KT_TASK_SLOTS; i++) {
        if (tasks[i].state == WAITING && tasks[i].waiting == t->pid)
            tasks[i].state = READY;
        if (tasks[i].state != FREE && tasks[i].parent == t->pid) {
            tasks[i].detached = 1;
            if (tasks[i].state == EXITED)
                tasks[i].state = FREE;
        }
    }
    t->state = t->detached ? FREE : EXITED;
    schedule();
    for (;;) {
    } /* No runnable continuation. */
}
void smp_timer_irq(void *frame) {
    u64 id = gic3 ? gic3_ack() : MMIO32(0x0801000c);
    if (id >= 1020)
        return;
    if (id == 27)
        arm_timer(clock_hz() / 50);
    if (gic3)
        gic3_eoi(id);
    else
        MMIO32(0x08010010) = id;
    if (id == 27) {
        irqs++;
        console_pump();
        display_status();
        net_poll();
        schedule();
    }
}
