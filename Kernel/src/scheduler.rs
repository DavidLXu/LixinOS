use alloc::boxed::Box;
use alloc::collections::VecDeque;
use alloc::vec::Vec;
use core::{
    arch::{asm, global_asm},
    ptr::{addr_of_mut, read_volatile, write_volatile},
};

global_asm!(include_str!("scheduler.S"));
unsafe extern "C" {
    fn smp_init(gic3: i32);
    pub(crate) fn smp_cpus() -> u32;
    pub(crate) fn smp_busy_ticks() -> u64;
    fn smp_cpu() -> u32;
    pub(crate) fn smp_idle_ticks(cpu: u32) -> u64;
    fn smp_idle_wait(delay: u64);
    fn smp_kernel_yield();
    fn smp_revoke_clock(cpu: u32);
    fn guest_schedule_switch(cpu: u32, slot: u32);
    fn swap_stack(save: *mut usize, resume: usize, fresh_top: usize);
}
pub const CAPACITY: usize = 64;
const STACK_BYTES: usize = 64 * 1024;
type ExitStatus = Result<u8, &'static str>;
type Entry = Box<dyn FnOnce() -> ExitStatus>;
#[derive(Clone, Copy, PartialEq)]
enum State {
    Free,
    Idle,
    Ready,
    Waiting(usize),
    Foreground(usize, u64),
    Channel(usize),
    Sleeping(u64),
    Exited(ExitStatus),
}
struct Task {
    state: State,
    core: usize,
    pid: usize,
    pane: usize,
    parent: usize,
    thread: bool,
    job: bool,
    watching: bool,
    stop: u8,
    background: bool,
    #[cfg(feature = "retina")]
    terminal: usize,
    sp: usize,
    ticks: u64,
    idle: u64,
    name: [u8;32],
    entry: Option<Entry>,
    stack: alloc::vec::Vec<u128>,
}
impl Task {
    const EMPTY: Self = Self {
        state: State::Free,
        core: 0,
        pid: 0,
        pane: 0,
        parent: 0,
        thread: false,
        job: false,
        watching: false,
        stop: 0,
        background: false,
        #[cfg(feature = "retina")]
        terminal: 0,
        sp: 0,
        ticks: 0,
        idle: 0,
        name: [0;32],
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
static mut TASKS: [Task; CAPACITY + 2] = [const { Task::EMPTY }; CAPACITY + 2];
#[cfg(feature = "split")]
static mut INPUT: [VecDeque<u8>; 2] = [const { VecDeque::new() }; 2];
static mut ACTIVE: [usize; 2] = [0, CAPACITY + 1];
static mut NEXT_PID: usize = 2;
#[cfg(feature = "split")]
static mut FOCUS: usize = 0;
static mut STARTED: usize = 0;
static mut RETIRED: Option<usize> = None;
static mut STARTUP_DEADLINE: u64 = 0;
static mut ONLINE: bool = false;
static mut START_TICKS: u64 = 0;
static mut LAST_IDLE: [u64; 2] = [0; 2];
static mut LAST_SWITCH: [u64; 2] = [0; 2];
static mut TIMER_INTERRUPTS:u64=0;
pub fn timer_interrupts()->u64 {unsafe {TIMER_INTERRUPTS}}
static mut GIC3: bool = false;
fn task(slot: usize) -> &'static mut Task {
    unsafe { &mut (*addr_of_mut!(TASKS))[slot] }
}
pub fn active() -> usize {
    unsafe { ACTIVE[core_id()] }
}

pub(crate) fn core_id() -> usize { unsafe { smp_cpu() as usize } }
fn choose_core() -> usize {
    if unsafe { smp_cpus() } == 1 { return 0; }
    let mut load = [0usize; 2];
    let mut children = [0usize; 2];
    let parent = pid();
    for i in 0..CAPACITY {
        let t = task(i);
        if matches!(t.state, State::Free | State::Exited(_)) { continue; }
        if t.parent == parent { children[t.core] += 1; }
        if t.state == State::Ready { load[t.core] += 1; }
    }
    if children[0] != children[1] { return usize::from(children[1] < children[0]); }
    if load[0] == load[1] { 1 - core_id() } else { usize::from(load[1] < load[0]) }
}
#[unsafe(no_mangle)]
extern "C" fn smp_bind_kernel() {
    #[cfg(feature = "split")]
    if active() < CAPACITY { crate::graphics::select_pane(pane()); }
}
#[unsafe(no_mangle)]
extern "C" fn smp_secondary_schedule() -> ! {
    unsafe { LAST_SWITCH[1] = crate::monitor::ticks(); }
    idle_scheduler()
}
fn idle_scheduler() -> ! {
    loop { schedule(); }
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
            State::Idle => "idle",
            State::Ready if unsafe { (*core::ptr::addr_of!(ACTIVE)).contains(&slot) } => "running",
            State::Ready => "ready",
            State::Waiting(_) | State::Foreground(_, _) | State::Channel(_) => "waiting",
            State::Sleeping(_) => "sleeping",
            State::Exited(_) => "exited",
        };
        let mode = if task.background { "background" } else { "foreground" };
        writeln!(output, "{} {} {state} {mode}", task.pid, task.parent).unwrap();
    }
    output
}
pub fn set_task_name(path: &str) {
    let name = &mut task(active()).name;
    name.fill(0);
    for (dst,src) in name[..31].iter_mut().zip(path.rsplit('/').next().unwrap_or("").bytes()) {
        *dst = if src.is_ascii_graphic() { src } else { b'?' };
    }
}
// ABI 29/9: wall ticks, idle ticks, RAM bytes; each row has six u64s and a 32-byte name.
pub fn resource_snapshot(out: &mut [u64]) -> usize {
    let now = crate::monitor::cpu_counters();
    out[..3].copy_from_slice(&[now.ticks,now.normalized_idle(),crate::inspect::ram_size().unwrap_or(0) as u64]);
    let mut n=3;
    for id in 0..CAPACITY {
        let t=task(id);
        let state=match t.state {
            State::Free=>continue, State::Idle=>continue, State::Ready if unsafe { ACTIVE[t.core] }==id=>1, State::Ready=>2,
            State::Sleeping(_)=>3, State::Exited(_)=>5, _=>4,
        };
        let mut cpu=t.ticks.saturating_sub(t.idle);
        if unsafe { ACTIVE[t.core] } == id { unsafe { cpu += (now.ticks-LAST_SWITCH[t.core]).saturating_sub(smp_idle_ticks(t.core as u32)-LAST_IDLE[t.core]); } }
        out[n..n+6].copy_from_slice(&[t.pid as u64,t.parent as u64,cpu,crate::user::private_bytes(id) as u64,state,t.background as u64]);
        for (i,bytes) in t.name.chunks_exact(8).enumerate() { out[n+6+i]=u64::from_le_bytes(bytes.try_into().unwrap()); }
        n+=10;
    }
    n*8
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
pub fn query_pid(value: u64) -> u64 {
    if value == 0 { return pid() as u64; }
    if value == u64::MAX { return task(crate::user::bank()).parent as u64; }
    if (0..CAPACITY).any(|i| !task(i).thread && task(i).state != State::Free && task(i).pid as u64 == value) { value } else { 0 }
}
pub fn pid() -> usize {
    task(crate::user::bank()).pid
}
pub fn ticks() -> u64 {
    unsafe { task(active()).ticks + crate::monitor::ticks().wrapping_sub(LAST_SWITCH[core_id()]) }
}
pub(crate) fn uptime_ticks() -> u64 {
    crate::monitor::ticks().wrapping_sub(unsafe { START_TICKS })
}
pub fn spawn(entry: Entry, pane: usize) -> Result<usize, &'static str> {
    spawn_task(entry, pane, false)
}
fn spawn_task(entry: Entry, pane: usize, thread: bool) -> Result<usize, &'static str> {
    let slots = if thread { crate::user::BANKS..CAPACITY } else { 1..crate::user::BANKS };
    let slot = slots.into_iter()
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
    if !thread { crate::user::setup_bank(slot)?; }
    unsafe extern "C" { fn id_inherit(child: u32, parent: u32); }
    unsafe { id_inherit(slot as u32,crate::user::bank() as u32); }
    *task(slot) = Task {
        state: State::Ready,
        core: choose_core(),
        pid,
        pane,
        // A login on another pane has its own controlling terminal.
        parent: if pane == self::pane() { self::pid() } else { 0 },
        background: background(),
        #[cfg(feature = "retina")]
        terminal: if background() || terminal_id() != 0 {
            terminal_id()
        } else {
            crate::graphics::terminal_id()
        },
        stack,
        entry: Some(entry),
        thread,
        ..Task::EMPTY
    };
    runnable(slot);
    Ok(pid)
}
unsafe extern "C" {
    fn kt_run(slot: u32) -> i64;
    pub(crate) fn kt_owner(slot: u32) -> u32;
}
#[unsafe(no_mangle)]
extern "C" fn kt_slot() -> u32 { active() as u32 }
#[unsafe(no_mangle)]
extern "C" fn kt_new() -> i64 {
    let owner = crate::user::bank();
    let Ok(tid) = spawn_task(Box::new(|| { unsafe { kt_run(active() as u32); } Ok(0) }), pane(), true) else { return -1; };
    let slot = (1..CAPACITY).find(|&i| task(i).pid == tid).unwrap();
    let mut load = [0usize; 2];
    load[task(owner).core] = 1;
    for i in 1..CAPACITY {
        if i != slot && task(i).thread && unsafe { kt_owner(i as u32) } as usize == owner { load[task(i).core] += 1; }
    }
    task(slot).core = usize::from(unsafe { smp_cpus() } == 2 && load[1] < load[0]);
    task(slot).parent = task(owner).pid;
    slot as i64
}
#[unsafe(no_mangle)]
extern "C" fn kt_id(slot: u32) -> i64 { task(slot as usize).pid as i64 }
#[unsafe(no_mangle)]
extern "C" fn kt_park(ticks: u64) { block(State::Sleeping(ticks)); }
#[unsafe(no_mangle)]
extern "C" fn kt_unpark(slot: u32) { unblock_slot(slot as usize); }
fn unblock_slot(i: usize) {
    let waiters = unsafe { &mut *addr_of_mut!(WAITERS) };
    if let Some(n) = waiters.iter().position(|&slot| slot == i) {
        waiters.swap_remove(n);
        runnable(i);
    }
}
#[unsafe(no_mangle)]
extern "C" fn kt_stop(slot: u32) {
    let i = slot as usize;
    let cpu = task(i).core;
    if unsafe { ACTIVE[cpu] } == i { unsafe { smp_revoke_clock(cpu as u32); } }
    task(i).stop = 137;
    unblock_slot(i);
}
#[unsafe(no_mangle)]
extern "C" fn kt_stopped() -> i32 { stopped() as i32 }
#[unsafe(no_mangle)]
extern "C" fn kt_sleep(ms: u32) { if ms == 0 { schedule(); } else { sleep_ms(ms as u64); } }

pub fn init() {
    for c in 0..2 {
        let idle = task(CAPACITY+c);
        idle.core = c;
        idle.state = State::Idle;
        idle.stack.resize(STACK_BYTES/16, 0);
    }
    task(0).state = State::Ready;
    task(0).pid = 1;
    unsafe {
        START_TICKS = crate::monitor::ticks();
        LAST_SWITCH[0] = START_TICKS;
        ONLINE = true;
        STARTUP_DEADLINE = START_TICKS + crate::monitor::seconds_to_ticks(1);
        GIC3 = read_volatile(0x0800_ffe8 as *const u32) >> 4 & 15 >= 3;
        if !GIC3 {
            write_volatile(0x0800_0100 as *mut u32, 1 << 27);
            write_volatile(0x0800_041b as *mut u8, 0x80);
            write_volatile(0x0800_0000 as *mut u32, 1);
            write_volatile(0x0801_0004 as *mut u32, 255);
            write_volatile(0x0801_0000 as *mut u32, 1);
        } else {
            // QEMU virt GICv3 redistributor for CPU 0. Virtual timer is PPI 27.
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
        smp_init(GIC3 as i32);
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
extern "C" fn smp_timer_irq(_frame: *mut u8) {
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
        #[cfg(feature = "retina")]
        crate::graphics::poll_window();
        checkpoint();
        #[cfg(feature = "retina")]
        crate::graphics::sample();
    }
}

// A task cannot free the stack it is returning on. The first continuation
// on the new stack releases both allocations, even before its parent waits.
fn collect_retired() {
    if let Some(slot) = unsafe { (&mut *addr_of_mut!(RETIRED)).take() } {
        if slot < crate::user::BANKS { crate::user::free_space(slot); }
        task(slot).stack = Vec::new();
    }
}
#[unsafe(no_mangle)]
extern "C" fn start_task() -> ! {
    collect_retired();
    let slot = active();
    if slot >= CAPACITY { idle_scheduler(); }
    let status = task(slot).entry.take().unwrap()();
    finish(status)
}
pub fn finish(status: ExitStatus) -> ! {
    let slot = active();
    if task(slot).thread { task(slot).parent = 0; } else { crate::user::release_image(); }
    let id = task(slot).pid;
    notify(|s| s == State::Waiting(id) || matches!(s, State::Foreground(child, _) if child == id));
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

pub fn job(pid: usize) {
    if let Some(i) = (1..CAPACITY).find(|&i| !task(i).thread && task(i).pid == pid && task(i).parent == self::pid()) {
        task(i).job = true;
    }
}

pub fn own_interrupt_key() { task(active()).job = false; }

pub fn stopped() -> u8 { task(active()).stop }

fn stop_tree(slot: usize, status: u8) {
    if matches!(task(slot).state, State::Free | State::Exited(_)) { return; }
    let cpu = task(slot).core;
    if unsafe { ACTIVE[cpu] } == slot { unsafe { smp_revoke_clock(cpu as u32); } }
    task(slot).stop = status;
    let waiters = unsafe { &mut *addr_of_mut!(WAITERS) };
    if let Some(i) = waiters.iter().position(|&id| id == slot) {
        waiters.swap_remove(i);
        runnable(slot);
    }
    let parent = task(slot).pid;
    for i in 1..CAPACITY {
        if task(i).parent == parent { stop_tree(i, status); }
    }
}

pub fn stop_child(pid: usize) -> bool {
    if let Some(i) = (1..CAPACITY).find(|&i| !task(i).thread && task(i).pid == pid && task(i).parent == self::pid()) {
        stop_tree(i, 137);
        true
    } else { false }
}

fn interrupt_jobs(owner: usize) {
    for i in 1..CAPACITY {
        if task(i).job && task(i).parent == owner { stop_tree(i, 130); }
    }
}

pub fn interrupt_reader() -> bool {
    #[cfg(feature = "retina")]
    if crate::graphics::editor_popup() { return false; }
    #[cfg(feature = "retina")]
    if crate::graphics::graphical_shortcuts() { return false; }
    let mut slot = active();
    while let Some(parent) = parent_slot(slot) {
        if task(slot).job && task(parent).watching {
            interrupt_jobs(task(parent).pid);
            return true;
        }
        slot = parent;
    }
    false
}

fn foreground_input(child: usize) -> bool {
    #[cfg(feature = "split")]
    if pane() != focus() { return false; }
    #[cfg(feature = "retina")]
    if crate::graphics::editor_popup() || task(child).terminal != crate::graphics::terminal_id() || crate::graphics::graphical_shortcuts() { return false; }
    // A nested shell owns input while waiting for its own foreground job.
    for i in 1..CAPACITY {
        if task(i).watching {
            let mut ancestor = Some(i);
            while let Some(slot) = ancestor {
                if slot == child { return false; }
                ancestor = parent_slot(slot);
            }
        }
    }
    crate::take_interrupt()
}

pub fn wait(pid: usize) -> Result<u8, &'static str> {
    let parent = self::pid();
    let slot = (1..CAPACITY)
        .find(|&i| !task(i).thread && task(i).pid == pid && task(i).parent == parent && task(i).state != State::Free)
        .ok_or("not a child process")?;
    loop {
        if let State::Exited(status) = task(slot).state {
            task(active()).watching = false;
            *task(slot) = Task::EMPTY;
            return status;
        }
        if task(slot).job && !background() && !crate::process::agent_noninteractive() {
            task(active()).watching = true;
            if foreground_input(slot) { interrupt_jobs(parent); }
            let deadline = crate::monitor::ticks() + crate::monitor::seconds_to_ticks(1) / 100;
            block(State::Foreground(pid, deadline));
        } else {
            block(State::Waiting(pid));
        }
    }
}

pub(crate) fn try_wait(pid: usize) -> Option<ExitStatus> {
    let parent = self::pid();
    let slot = (1..CAPACITY).find(|&i| !task(i).thread && task(i).pid == pid && task(i).parent == parent)?;
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
    if let Some(slot) = (1..CAPACITY).find(|&i| !task(i).thread && task(i).pid == pid && task(i).parent == self::pid())
    {
        task(slot).parent = 0;
        task(slot).background = true;
    }
}
pub fn schedule() {
    let old = active();
    for step in 1..CAPACITY {
        let i = (old + step) % CAPACITY;
        if matches!(task(i).state, State::Exited(_)) && task(i).parent == 0 && i != old {
            *task(i) = Task::EMPTY;
        }
    }
    let cpu = core_id();
    let now = crate::monitor::ticks();
    notify(|s| matches!(s,State::Sleeping(deadline) | State::Foreground(_,deadline) if deadline<=now));
    let ready = unsafe { &mut *addr_of_mut!(READY) };
    let next = ready.iter().position(|&i| task(i).core == cpu).and_then(|p| ready.remove(p));
    let new = match next {
        Some(id) if id != old => id,
        _ if task(old).state == State::Ready => { unsafe { smp_kernel_yield(); } return; }
        _ if old < CAPACITY => CAPACITY + cpu,
        _ => { unsafe { smp_idle_wait(crate::monitor::seconds_to_ticks(1)/1000); } return; }
    };
    if task(old).state == State::Ready && !unsafe { (&*addr_of_mut!(READY)).contains(&old) } {
        runnable(old);
    }
    let now = crate::monitor::ticks();
    unsafe {
        task(old).ticks += now.wrapping_sub(LAST_SWITCH[core_id()]);
        let idle = smp_idle_ticks(cpu as u32);
        task(old).idle += idle - LAST_IDLE[cpu];
        LAST_IDLE[cpu] = idle;
        LAST_SWITCH[cpu] = now;
        if matches!(task(old).state, State::Exited(_)) {
            RETIRED = Some(old);
        }
        ACTIVE[cpu] = new;
        guest_schedule_switch(cpu as u32, new as u32);
        smp_bind_kernel();
        crate::user::select_bank(if new < CAPACITY { kt_owner(new as u32) as usize } else { 0 });
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
    if now.wrapping_sub(unsafe { LAST_SWITCH[core_id()] }) >= crate::monitor::seconds_to_ticks(1) / 100 {
        schedule();
    }
}
#[cfg(feature = "split")]
pub fn take_key_interrupt() -> bool {
    checkpoint();
    let queue = unsafe { &mut *addr_of_mut!(INPUT[pane()]) };
    queue.iter().position(|&k| k == 3).and_then(|i| queue.remove(i)).is_some()
}
#[cfg(feature = "split")]
pub fn input() -> Option<u8> {
    unsafe { (&mut *addr_of_mut!(INPUT[pane()])).pop_front() }
}
