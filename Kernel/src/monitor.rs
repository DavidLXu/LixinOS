use crate::{HEAP, print};
use alloc::format;
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

pub fn show(start: u64, files: impl Fn() -> (usize, usize, usize)) {
    let freq = seconds_to_ticks(1);
    let window = cfg!(feature = "window-console");
    if !window {
        print("\x1b[?1049h\x1b[2J");
    }
    loop {
        let now = ticks();
        let (count, dirs, bytes) = files();
        let (used, free) = heap_usage();
        let segments = if cfg!(feature = "split") { 8 } else { 32 };
        let bars = segments * used / (used + free);
        if !window {
            print("\x1b[H");
        }
        #[cfg(feature = "split")]
        let stack_kib = if crate::panes::active() == 1 { 32 } else { 16 };
        #[cfg(not(feature = "split"))]
        let stack_kib = 16;
        let frame = format!(
            "LixinOS   htop                         q: quit\n\nUptime  {} s          CPUs  1\x1b[K\nGuest RAM  128 MiB    Kernel stack  {stack_kib} KiB\nHeap [{}{}] {} B used / {} B free\x1b[K\nFilesystem  {} files, {} dirs, {} bytes\x1b[K\n\nCONTEXT       STATE       ACTIVITY\nKernel shell  foreground  htop (console polling)\n",
            (now - start) / freq,
            "#".repeat(bars),
            "-".repeat(segments - bars),
            used,
            free,
            count,
            dirs,
            bytes
        );
        print(&frame);
        #[cfg(feature = "retina")]
        crate::graphics::waiting(true);
        while ticks().wrapping_sub(now) < freq {
            if matches!(crate::poll_byte(), Some(b'q' | b'Q' | 27)) {
                #[cfg(feature = "retina")]
                crate::graphics::waiting(false);
                if !window {
                    print("\x1b[?1049l");
                }
                return;
            }
            core::hint::spin_loop();
        }
        #[cfg(feature = "retina")]
        crate::graphics::waiting(false);
        // QEMU's virtual console lacks an alternate screen. Refresh in place
        // below the command so its existing terminal history stays visible.
        if window {
            print(&format!("\x1b[{}A", frame.lines().count()));
        }
    }
}
