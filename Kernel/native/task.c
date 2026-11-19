#include "native.h"

enum { FREE, READY, WAITING, EXITED, PAUSED };
struct task {
    u64 stack, pid, parent, waiting;
    int state, detached;
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

void tasks_init(void) {
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
        .pid = next_pid++, .parent = tasks[current].pid, .state = READY, .detached = detached};
    return tasks[slot].pid;
}
unsigned task_active(void) { return current; }
u64 task_pid(void) { return tasks[current].pid; }
void schedule(void) {
    unsigned old = current;
    for (unsigned k = 1; k < NATIVE_TASKS; k++) {
        unsigned id = (old + k) % NATIVE_TASKS;
        if (tasks[id].state != READY)
            continue;
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
long task_wait_mode(u64 pid, int nohang) {
    for (unsigned i = 1; i < NATIVE_TASKS; i++) {
        struct task *child = &tasks[i];
        if (child->state == FREE || child->pid != pid || child->parent != task_pid() ||
            child->detached)
            continue;
        if (child->state != EXITED) {
            if (nohang) return -2;
            tasks[current].state = WAITING;
            tasks[current].waiting = pid;
            schedule();
            tasks[current].state = READY;
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
    t->status = run_task(current);
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
