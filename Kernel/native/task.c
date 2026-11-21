#include "native.h"

enum { FREE, READY, WAITING, EXITED, PAUSED };
struct task {
    u64 stack, pid, parent, waiting, cpu;
    int state, detached, job, watching, stop;
    unsigned terminal;
    long status;
};
static struct task tasks[NATIVE_TASKS];
static u8 stacks[NATIVE_TASKS][128 * 1024] __attribute__((aligned(16)));
static unsigned current, gic3;
static u64 next_pid = 2, irqs;
u64 timer_count(void) { return irqs; }
long task_list(char *out) {
    char *p = add_text(out, "PID PPID STATE MODE\n");
    for (unsigned i = 0; i < NATIVE_TASKS; i++) {
        const struct task *t = &tasks[i];
        if (!t->state)
            continue;
        p = add_number(p, t->pid);
        *p++ = ' ';
        p = add_number(p, t->parent);
        p = add_text(p, t->state == WAITING  ? " waiting "
                        : t->state == EXITED ? " exited "
                        : i == current       ? " running "
                                             : " ready ");
        p = add_text(p, t->detached ? "background\n" : "foreground\n");
    }
    return p - out;
}
extern void swap_task(u64 *old, u64 resume, u64 fresh_top);
extern void arm_timer(u64 ticks), gic3_enable(void), gic3_eoi(u64 id);
extern u64 gic3_ack(void);
#define MMIO32(p) (*(volatile u32 *)(u64)(p))

static u64 busy_start;
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
    arm_timer(clock_hz() / 50);
}
static u64 idle, sample_at, sample_idle;
static unsigned busy;
u64 idle_ticks(void) { return idle; }
unsigned cpu_percent(void) {
    u64 now = clock_ticks(), delta = now - sample_at;
    if (delta >= clock_hz() / 2) {
        u64 waited = idle - sample_idle;
        busy = waited >= delta ? 0 : 100 - waited * 100 / delta;
        sample_at = now;
        sample_idle = idle;
    }
    return busy;
}
void idle_slice(void) {
    display_status();
    for (unsigned i = 0; i < NATIVE_TASKS; i++)
        if (i != current && tasks[i].state == READY) {
            schedule();
            return;
        }
    u64 start = clock_ticks(), delay = clock_hz() / 4000;
    while (clock_ticks() - start < delay)
        asm volatile(".long 0xd503203f") /* yield */;
    idle += clock_ticks() - start;
}
int task_slot(void) {
    for (unsigned i = 1; i < NATIVE_TASKS; i++)
        if (tasks[i].state == FREE)
            return i;
    return -1;
}
long task_create(unsigned slot, int detached) {
    if (!next_pid || tasks[slot].state != FREE)
        return -1;
    tasks[slot] = (struct task){
        .pid = next_pid++, .parent = tasks[current].pid, .state = READY, .detached = detached,
        .terminal = task_terminal() ? task_terminal() : popup_terminal()};

    return tasks[slot].pid;
}
unsigned task_active(void) { return current; }
u64 task_pid(void) { return tasks[current].pid; }
unsigned task_terminal(void) { return tasks[current].terminal; }
long task_resources(u64 *out) {
    u64 now=clock_ticks(), busy=now-idle_ticks();
    out[0]=now; out[1]=idle_ticks(); out[2]=native_ram_bytes();
    unsigned n=3;
    for(unsigned i=0;i<NATIVE_TASKS;i++) {
        struct task *t=&tasks[i];
        if(!t->state) continue;
        out[n]=t->pid; out[n+1]=t->parent;
        out[n+2]=t->cpu+(i==current ? busy-busy_start : 0);
        out[n+3]=process_private_bytes(i);
        out[n+4]=t->state==EXITED?5:t->state==WAITING||t->state==PAUSED?4:i==current?1:2;
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
    unsigned old = current;
    for (unsigned k = 1; k < NATIVE_TASKS; k++) {
        unsigned id = (old + k) % NATIVE_TASKS;
        if (tasks[id].state != READY)
            continue;
        u64 busy=clock_ticks()-idle_ticks();
        tasks[old].cpu+=busy-busy_start;
        busy_start=busy;
        current = id;
        select_bank(id);
        swap_task(&tasks[old].stack, tasks[id].stack, (u64)&stacks[id][sizeof(stacks[id])]);
        return;
    }
}
void task_pause(void) {
    for (unsigned i = 0; i < NATIVE_TASKS; i++) {
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
    t->stop = status;
    if (t->state == PAUSED) t->state = READY;
    for (unsigned i = 1; i < NATIVE_TASKS; i++)
        if (tasks[i].parent == t->pid && !tasks[i].detached) stop_tree(i, status);
}
int task_stop_child(u64 pid) {
    for (unsigned i = 1; i < NATIVE_TASKS; i++)
        if (tasks[i].state != FREE && tasks[i].pid == pid && tasks[i].parent == task_pid()
            && !tasks[i].detached) { stop_tree(i,137); return 0; }
    return -1;
}
static void interrupt_jobs(u64 owner) {
    for (unsigned i = 1; i < NATIVE_TASKS; i++)
        if (tasks[i].job && !tasks[i].detached && tasks[i].parent == owner) stop_tree(i,130);
}
static int parent_slot(unsigned slot) {
    if (tasks[slot].detached) return -1;
    for (unsigned i = 0; i < NATIVE_TASKS; i++)
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
    for (unsigned i = 1; i < NATIVE_TASKS; i++) {
        if (!tasks[i].watching) continue;
        for (int p = i; p >= 0; p = parent_slot(p)) if (p == (int)child) return 0;
    }
    return 1;
}
long task_wait_mode(u64 pid, int nohang) {
    for (unsigned i = 1; i < NATIVE_TASKS; i++) {
        struct task *child = &tasks[i];
        if (child->state == FREE || child->pid != pid || child->parent != task_pid() ||
            child->detached)
            continue;
        if (child->state != EXITED) {
            if (nohang) return -2;
            if (child->job && !tasks[current].detached && !tasks[current].stop) {
                tasks[current].watching = 1;
                while (child->job && child->state != EXITED && child->state != PAUSED) {
                    if (owns_console(i) && console_interrupt()) interrupt_jobs(task_pid());
                    schedule();
                }
                tasks[current].watching = 0;
            }
            if (child->state != EXITED && child->state != PAUSED) {
                tasks[current].state = WAITING;
                tasks[current].waiting = pid;
                schedule();
                tasks[current].state = READY;
            }
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
void task_start(void) {
    struct task *t = &tasks[current];
    long status = run_task(current);
    /* A child that could not execute still has an exit status for waitpid. */
    t->status = status < 0 ? 126 : status;
    for (unsigned i = 0; i < NATIVE_TASKS; i++) {
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
void timer_interrupt(void) {
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
