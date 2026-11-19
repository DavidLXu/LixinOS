//! Time-sliced continuations on one vCPU. Input focus is independent of execution.
use alloc::boxed::Box;
use alloc::collections::VecDeque;
use alloc::vec::Vec;
use core::{
    arch::{asm, global_asm},
    ptr::{addr_of_mut, read_volatile, write_volatile},
};

global_asm!(include_str!("scheduler.S"));
unsafe extern "C" {
    fn swap_stack(save: *mut usize, resume: usize, fresh_top: usize);
}
pub const CAPACITY: usize = 16;
const STACK_BYTES: usize = 128 * 1024;
type ExitStatus = Result<u8, &'static str>;
type Entry = Box<dyn FnOnce() -> ExitStatus>;
#[derive(Clone, Copy, PartialEq)]
enum State {
    Free,
    Ready,
    Waiting(usize),
    Channel(usize),
    Sleeping(u64),
    Exited(ExitStatus),
}
struct Task {
    state: State,
    pid: usize,
    pane: usize,
    parent: usize,
    background: bool,
    #[cfg(feature = "retina")]
    terminal: usize,
    sp: usize,
    ticks: u64,
    entry: Option<Entry>,
    stack: alloc::vec::Vec<u128>,
}
impl Task {
    const EMPTY: Self = Self {
        state: State::Free,
        pid: 0,
        pane: 0,
        parent: 0,
        background: false,
        #[cfg(feature = "retina")]
        terminal: 0,
        sp: 0,
        ticks: 0,
        entry: None,
        stack: alloc::vec::Vec::new(),
    };
}
// The running task is outside READY. A blocked slot is exclusively in WAITERS
// until its child, pipe peer, or device-poll deadline makes it runnable.
static mut READY: VecDeque<usize> = VecDeque::new();
static mut WAITERS: Vec<usize> = Vec::new();
fn runnable(slot: usize) {
    task(slot).state = State::Ready;
    unsafe {
        (&mut *addr_of_mut!(READY)).push_back(slot);
    }
}
fn block(state: State) {
    let slot = active();
    task(slot).state = state;
    unsafe {
        (&mut *addr_of_mut!(WAITERS)).push(slot);
    }
    schedule();
}
fn notify(mut condition: impl FnMut(State) -> bool) {
    let waiters = unsafe { &mut *addr_of_mut!(WAITERS) };
    waiters.retain(|&slot| {
        if condition(task(slot).state) {
            runnable(slot);
            false
        } else {
            true
        }
    });
}
static mut TASKS: [Task; CAPACITY] = [const { Task::EMPTY }; CAPACITY];
#[cfg(feature = "split")]
static mut INPUT: [VecDeque<u8>; 2] = [const { VecDeque::new() }; 2];
static mut ACTIVE: usize = 0;
static mut NEXT_PID: usize = 2;
#[cfg(feature = "split")]
static mut FOCUS: usize = 0;
static mut STARTED: usize = 0;
static mut RETIRED: Option<usize> = None;
static mut STARTUP_DEADLINE: u64 = 0;
static mut ONLINE: bool = false;
static mut START_TICKS: u64 = 0;
static mut LAST_SWITCH: u64 = 0;
static mut TIMER_INTERRUPTS:u64=0;
pub fn timer_interrupts()->u64 {unsafe {TIMER_INTERRUPTS}}
static mut GIC3: bool = false;
fn task(slot: usize) -> &'static mut Task {
    unsafe { &mut (*addr_of_mut!(TASKS))[slot] }
}
pub fn active() -> usize {
    unsafe { ACTIVE }
}

pub(crate) fn parent_slot(slot: usize) -> Option<usize> {
    let parent = task(slot).parent;
    (parent != 0).then(|| (0..CAPACITY).find(|&i| task(i).pid == parent)).flatten()
}

pub(crate) fn process_list() -> alloc::string::String {
    use core::fmt::Write;
    let mut output = alloc::string::String::from("PID PPID STATE MODE\n");
    for slot in 0..CAPACITY {
        let task = task(slot);
        let state = match task.state {
            State::Free => continue,
            State::Ready if slot == active() => "running",
            State::Ready => "ready",
            State::Waiting(_) | State::Channel(_) => "waiting",
            State::Sleeping(_) => "sleeping",
            State::Exited(_) => "exited",
        };
        let mode = if task.background { "background" } else { "foreground" };
        writeln!(output, "{} {} {state} {mode}", task.pid, task.parent).unwrap();
    }
    output
}
pub fn pane() -> usize {
    task(active()).pane
}
#[cfg(feature = "split")]
pub fn focus() -> usize {
    unsafe { FOCUS }
}
#[cfg(feature = "retina")]
pub fn terminal_id() -> usize {
    task(active()).terminal
}
pub fn background() -> bool {
    task(active()).background
}
pub fn pid() -> usize {
    task(active()).pid
}
pub fn ticks() -> u64 {
    unsafe { task(active()).ticks + crate::monitor::ticks().wrapping_sub(LAST_SWITCH) }
}
pub(crate) fn uptime_ticks() -> u64 {
    crate::monitor::ticks().wrapping_sub(unsafe { START_TICKS })
}
#[cfg(feature = "retina")]
pub fn stack_bounds() -> (usize, usize) {
    let stack = &task(active()).stack;
    let base = stack.as_ptr() as usize;
    (base, base + stack.len() * 16)
}
pub fn spawn(entry: Entry, pane: usize) -> Result<usize, &'static str> {
    let slot = (1..CAPACITY)
        .find(|&i| task(i).state == State::Free)
        .ok_or("process table full")?;
    let mut stack = alloc::vec::Vec::new();
    stack
        .try_reserve_exact(STACK_BYTES / 16)
        .map_err(|_| "out of memory for process stack")?;
    stack.resize(STACK_BYTES / 16, 0);
    let pid = unsafe {
        let id = NEXT_PID;
        NEXT_PID = id.checked_add(1).ok_or("PID limit")?;
        id
    };
    crate::user::setup_bank(slot)?;
    *task(slot) = Task {
        state: State::Ready,
        pid,
        pane,
        // A login on another pane has its own controlling terminal.
        parent: if pane == self::pane() { self::pid() } else { 0 },
        background: background(),
        #[cfg(feature = "retina")]
        terminal: if background() {
            terminal_id()
        } else {
            crate::graphics::terminal_id()
        },
        stack,
        entry: Some(entry),
        ..Task::EMPTY
    };
    runnable(slot);
    Ok(pid)
}
pub fn init() {
    task(0).state = State::Ready;
    task(0).pid = 1;
    unsafe {
        START_TICKS = crate::monitor::ticks();
        LAST_SWITCH = START_TICKS;
        ONLINE = true;
        STARTUP_DEADLINE = LAST_SWITCH + crate::monitor::seconds_to_ticks(1);
        GIC3 = read_volatile(0x0800_ffe8 as *const u32) >> 4 & 15 >= 3;
        if !GIC3 {
            write_volatile(0x0800_0100 as *mut u32, 1 << 27);
            write_volatile(0x0800_041b as *mut u8, 0x80);
            write_volatile(0x0800_0000 as *mut u32, 1);
            write_volatile(0x0801_0004 as *mut u32, 255);
            write_volatile(0x0801_0000 as *mut u32, 1);
        } else {
            // QEMU virt GICv3, single redistributor. Virtual timer is PPI 27.
            let waker = 0x080a_0014 as *mut u32;
            write_volatile(waker, read_volatile(waker) & !2);
            while read_volatile(waker) & 4 != 0 {
                core::hint::spin_loop();
            }
            let group = 0x080b_0080 as *mut u32;
            write_volatile(group, read_volatile(group) | (1 << 27));
            write_volatile(0x080b_041b as *mut u8, 0x80);
            write_volatile(0x080b_0100 as *mut u32, 1 << 27);
            let dist = 0x0800_0000 as *mut u32;
            write_volatile(dist, read_volatile(dist) | 3);
            asm!("dsb sy", "mrs x9, ICC_SRE_EL1", "orr x9,x9,#1", "msr ICC_SRE_EL1,x9", "isb",
             "mov x9,#255", "msr ICC_PMR_EL1,x9","mov x9,#1","msr ICC_IGRPEN1_EL1,x9","isb",out("x9") _);
        }
        rearm();
        asm!("mov x9,#1","msr cntv_ctl_el0,x9","isb",out("x9") _);
    }
}

fn rearm() {
    let period = crate::monitor::seconds_to_ticks(1) / 50;
    unsafe {
        asm!("msr cntv_tval_el0, {}", "isb", in(reg)period);
    }
}

#[unsafe(no_mangle)]
extern "C" fn task_timer_irq() {
    let id: u64;
    unsafe {
        if GIC3 {
            asm!("mrs {}, ICC_IAR1_EL1",out(reg)id);
        } else {
            id = read_volatile(0x0801_000c as *const u32) as u64;
        }
    }
    if id >= 1020 {
        return;
    }
    if id == 27 {
        unsafe { TIMER_INTERRUPTS+=1; }
        rearm();
    }
    // Complete the IRQ before suspending its exception frame.
    unsafe {
        if GIC3 {
            asm!("msr ICC_EOIR1_EL1, {}", "isb",in(reg)id);
        } else {
            write_volatile(0x0801_0010 as *mut u32, id as u32);
        }
    }
    if id == 27 {
        checkpoint();
        #[cfg(feature = "retina")]
        crate::graphics::sample();
    }
}

// A task cannot free the stack it is returning on. The first continuation
// on the new stack releases both allocations, even before its parent waits.
fn collect_retired() {
    if let Some(slot) = unsafe { (&mut *addr_of_mut!(RETIRED)).take() } {
        crate::user::free_space(slot);
        task(slot).stack = Vec::new();
    }
}
#[unsafe(no_mangle)]
extern "C" fn start_task() -> ! {
    collect_retired();
    let slot = active();
    let status = task(slot).entry.take().unwrap()();
    finish(status)
}
pub fn finish(status: ExitStatus) -> ! {
    let slot = active();
    crate::user::release_image();
    let id = pid();
    notify(|s| s == State::Waiting(id));
    for i in 0..CAPACITY {
        if task(i).parent == id {
            task(i).parent = 0;
            task(i).background = true;
        }
    }
    task(slot).state = State::Exited(status);
    schedule();
    crate::idle()
}

pub fn wait(pid: usize) -> Result<u8, &'static str> {
    let parent = self::pid();
    let slot = (1..CAPACITY)
        .find(|&i| task(i).pid == pid && task(i).parent == parent && task(i).state != State::Free)
        .ok_or("not a child process")?;
    loop {
        if let State::Exited(status) = task(slot).state {
            *task(slot) = Task::EMPTY;
            return status;
        }
        block(State::Waiting(pid));
    }
}

pub(crate) fn try_wait(pid: usize) -> Option<ExitStatus> {
    let parent = self::pid();
    let slot = (1..CAPACITY).find(|&i| task(i).pid == pid && task(i).parent == parent)?;
    if let State::Exited(status) = task(slot).state {
        *task(slot) = Task::EMPTY;
        return Some(status);
    }
    None
}
pub fn sleep_ms(ms: u64) {
    if !unsafe { ONLINE } {
        return;
    }
    block(State::Sleeping(
        crate::monitor::ticks() + crate::monitor::seconds_to_ticks(1) / 1000 * ms,
    ));
}
pub fn park(channel: usize) {
    block(State::Channel(channel));
}
pub fn wake(channel: usize) {
    notify(|s| s == State::Channel(channel));
}
pub fn detach(pid: usize) {
    if let Some(slot) = (1..CAPACITY).find(|&i| task(i).pid == pid && task(i).parent == self::pid())
    {
        task(slot).parent = 0;
        task(slot).background = true;
    }
}
pub fn schedule() {
    let old = active();
    for step in 1..CAPACITY {
        let i = (old + step) % CAPACITY;
        if matches!(task(i).state, State::Exited(_)) && task(i).parent == 0 {
            *task(i) = Task::EMPTY;
        }
    }
    let new = loop {
        let now = crate::monitor::ticks();
        notify(|s| matches!(s,State::Sleeping(deadline) if deadline<=now));
        if let Some(next) = unsafe { (&mut *addr_of_mut!(READY)).pop_front() } {
            if next == old {
                return;
            }
            break next;
        }
        if task(old).state == State::Ready {
            return;
        }
        // IRQs stay masked in EL1; sleep until the next virtual-timer poll.
        let delay = crate::monitor::seconds_to_ticks(1) / 1000;
        let idle_start = crate::monitor::ticks();
        unsafe {
            asm!("msr cntv_tval_el0, {}","isb","wfi", in(reg)delay);
        }
        crate::monitor::record_idle_since(idle_start);
    };
    if task(old).state == State::Ready && !unsafe { (&*addr_of_mut!(READY)).contains(&old) } {
        runnable(old);
    }
    let now = crate::monitor::ticks();
    unsafe {
        task(old).ticks += now.wrapping_sub(LAST_SWITCH);
        LAST_SWITCH = now;
        if matches!(task(old).state, State::Exited(_)) {
            RETIRED = Some(old);
        }
        ACTIVE = new;
        #[cfg(feature = "split")]
        if task(old).pane != task(new).pane {
            crate::graphics::switch_pane();
        }
        crate::user::select_bank(new);
        let stack = &task(new).stack;
        let top = stack.as_ptr() as usize + stack.len() * 16;
        swap_stack(addr_of_mut!(task(old).sp), task(new).sp, top);
        collect_retired();
    }
}
#[cfg(feature = "split")]
pub fn ready() {
    unsafe {
        STARTED += 1;
    }
    schedule();
}

pub fn checkpoint() {
    if !unsafe { ONLINE } {
        return;
    }
    #[cfg(feature = "split")]
    while let Some(byte) = crate::graphics::key() {
        if byte == crate::keyboard::SWITCH_PANE {
            unsafe {
                FOCUS ^= 1;
            }
            crate::graphics::focus_changed();
            continue;
        }
        let pending = unsafe { &mut *addr_of_mut!(INPUT[focus()]) };
        if pending.len() < 512 {
            pending.push_back(byte);
        }
    }
    if cfg!(feature = "split")
        && unsafe { STARTED < 2 && crate::monitor::ticks() < STARTUP_DEADLINE }
    {
        return;
    }
    let now = crate::monitor::ticks();
    if now.wrapping_sub(unsafe { LAST_SWITCH }) >= crate::monitor::seconds_to_ticks(1) / 100 {
        schedule();
    }
}
#[cfg(feature = "split")]
pub fn input() -> Option<u8> {
    unsafe { (&mut *addr_of_mut!(INPUT[pane()])).pop_front() }
}
