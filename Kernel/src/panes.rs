//! Two foreground continuations. Kernel switches happen only at explicit
//! input checkpoints. EL0 timer IRQs also reach a checkpoint for pure compute.
use alloc::{boxed::Box, collections::VecDeque};
use core::{
    arch::{asm, global_asm},
    ptr::{addr_of_mut, read_volatile, write_volatile},
};

global_asm!(include_str!("panes.S"));
unsafe extern "C" {
    fn swap_stack(save: *mut usize, resume: usize, fresh_top: usize);
}
const STACK_BYTES: usize = 128 * 1024;
#[repr(align(16))]
struct Stack([u8; STACK_BYTES]);
static mut RIGHT_STACK: Stack = Stack([0; STACK_BYTES]);
static mut SAVED_SP: [usize; 2] = [0; 2];
static mut START: Option<Box<dyn FnOnce()>> = None;
static mut INPUT: [VecDeque<u8>; 2] = [const { VecDeque::new() }; 2];
static mut ACTIVE: usize = 0;
static mut LAST_SWITCH: u64 = 0;
static mut RUN_TICKS: [u64; 2] = [0; 2];

pub fn right_stack_bounds() -> (usize, usize) {
    let bottom = core::ptr::addr_of!(RIGHT_STACK) as usize;
    (bottom, bottom + STACK_BYTES)
}

pub fn active() -> usize {
    unsafe { ACTIVE }
}
pub fn ticks() -> u64 {
    unsafe { RUN_TICKS[ACTIVE] + crate::monitor::ticks().wrapping_sub(LAST_SWITCH) }
}

pub fn init(right: Box<dyn FnOnce()>) {
    unsafe {
        *addr_of_mut!(START) = Some(right);
        LAST_SWITCH = crate::monitor::ticks();
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
extern "C" fn pane_irq() {
    let id: u64;
    unsafe {
        asm!("mrs {}, ICC_IAR1_EL1",out(reg)id);
    }
    if id >= 1020 {
        return;
    }
    if id == 27 {
        rearm();
    }
    // Complete the IRQ before suspending its exception frame.
    unsafe {
        asm!("msr ICC_EOIR1_EL1, {}", "isb",in(reg)id);
    }
    if id == 27 {
        checkpoint();
        crate::graphics::sample();
    }
}

#[unsafe(no_mangle)]
extern "C" fn start_right() -> ! {
    let run = unsafe { (&mut *addr_of_mut!(START)).take().unwrap() };
    run();
    crate::idle()
}

pub fn switch() {
    let old = active();
    let new = old ^ 1;
    let now = crate::monitor::ticks();
    unsafe {
        RUN_TICKS[old] += now.wrapping_sub(LAST_SWITCH);
        LAST_SWITCH = now;
        ACTIVE = new;
        crate::graphics::switch_pane();
        crate::user::select_bank(new);
        let top = addr_of_mut!(RIGHT_STACK.0).cast::<u8>().add(STACK_BYTES) as usize;
        swap_stack(addr_of_mut!(SAVED_SP[old]), SAVED_SP[new], top);
    }
}

pub fn checkpoint() {
    while let Some(byte) = crate::graphics::key() {
        if byte == crate::keyboard::SWITCH_PANE {
            switch();
            return;
        }
        let pending = unsafe { &mut *addr_of_mut!(INPUT[active()]) };
        if pending.len() < 512 {
            pending.push_back(byte);
        }
    }
}

pub fn input() -> Option<u8> {
    checkpoint();
    unsafe { (&mut *addr_of_mut!(INPUT[active()])).pop_front() }
}
