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

#[derive(Clone, Copy)]
pub(crate) struct CpuCounters {
    pub ticks: u64,
    pub idle_ticks: u64,
    secondary_busy: u64,
}

impl CpuCounters {
    #[cfg(feature = "retina")]
    pub const ZERO: Self = Self { ticks: 0, idle_ticks: 0, secondary_busy: 0 };

    // Keep the resource snapshot ABI in wall-clock units.
    pub fn normalized_idle(self) -> u64 {
        if unsafe { crate::scheduler::smp_cpus() } == 1 { return self.idle_ticks; }
        (self.idle_ticks + self.ticks.saturating_sub(self.secondary_busy)) / 2
    }

    pub fn busy_percent(self, start: Self) -> Option<u64> {
        let elapsed = self.ticks.wrapping_sub(start.ticks);
        let idle = self.idle_ticks.wrapping_sub(start.idle_ticks);
        let remote = (self.secondary_busy - start.secondary_busy).min(elapsed);
        let busy = elapsed.saturating_sub(idle) + remote;
        let elapsed = elapsed * u64::from(unsafe { crate::scheduler::smp_cpus() });
        let rounded = u128::from(busy) * 100 + u128::from(elapsed / 2);
        (elapsed != 0).then(|| (rounded / u128::from(elapsed)) as u64)
    }
}

pub(crate) fn cpu_counters() -> CpuCounters {
    unsafe extern "C" { fn smp_idle_ticks(cpu: u32) -> u64; }
    CpuCounters { ticks: ticks(), idle_ticks: unsafe { smp_idle_ticks(0) },
        secondary_busy: unsafe { crate::scheduler::smp_busy_ticks() } }
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
        crate::scheduler::stack_bounds(),
    ];
    stacks.into_iter().find(|&(bottom, top)| (bottom..=top).contains(&sp))
        .map(|(bottom, top)| (top - sp, top - bottom))
}
