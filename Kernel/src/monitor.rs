use crate::HEAP;
use core::{arch::asm, ptr::addr_of};

pub fn ticks() -> u64 {
    let n;
    unsafe { asm!("mrs {}, cntvct_el0",out(reg)n) }
    n
}

pub fn seconds_to_ticks(seconds: u64) -> u64 {
    let freq: u64;
    unsafe { asm!("mrs {}, cntfrq_el0",out(reg)freq) };
    seconds * freq
}

pub fn heap_usage() -> (usize, usize) {
    let heap = unsafe { &*HEAP.0.get() };
    (heap.used(), heap.free())
}

pub fn kernel_span_bytes() -> usize {
    unsafe extern "C" {
        static __kernel_start: u8;
        static __stack_top: u8;
    }
    addr_of!(__stack_top) as usize - addr_of!(__kernel_start) as usize
}

/// Sample the kernel stack currently executing this code, including during a pane switch.
#[cfg(feature = "retina")]
pub fn kernel_stack_usage() -> Option<(usize, usize)> {
    unsafe extern "C" {
        static __stack_bottom: u8;
        static __stack_top: u8;
    }
    let sp: usize;
    unsafe { asm!("mov {}, sp", out(reg) sp, options(nostack, nomem, preserves_flags)); }
    let stacks = [
        (addr_of!(__stack_bottom) as usize, addr_of!(__stack_top) as usize),
        #[cfg(feature = "split")]
        crate::panes::right_stack_bounds(),
    ];
    stacks.into_iter().find(|&(bottom, top)| (bottom..=top).contains(&sp))
        .map(|(bottom, top)| (top - sp, top - bottom))
}
