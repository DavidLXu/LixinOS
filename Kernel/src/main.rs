#![no_std]
#![no_main]

extern crate alloc;
mod boot_package;
mod agent;
mod agent_system;
mod agent_tools;
#[cfg(feature = "retina")]
mod clipboard;
mod disk;
mod permissions;
mod foreground;
mod fs_batch;
#[cfg(feature = "retina")]
mod graphics;
mod inspect;
mod keyboard;
mod monitor;
mod net;
mod entropy;
#[cfg(feature = "ssh-mux")]
mod serial_mux;
mod scheduler;
mod pipe;
mod local_socket;
mod shared_memory;
mod process;
mod procfs;
mod git;
mod user;
mod utility;

use core::alloc::{GlobalAlloc, Layout};
use core::arch::{asm, global_asm};
use core::cell::UnsafeCell;
use core::panic::PanicInfo;
use core::ptr::NonNull;
use core::ptr::{read_volatile, write_volatile};
use linked_list_allocator::Heap;

global_asm!(
    ".section .text.boot, \"ax\"",
    ".global _start",
    "_start:",
    "msr daifset, #0xf",
    "adrp x0, __stack_top",
    "add x0, x0, :lo12:__stack_top",
    "mov sp, x0",
    "b kernel_main",
);

// First PL011 UART on QEMU's virt board.
const UART_BASE: usize = 0x0900_0000;
const UART_DATA: *mut u32 = UART_BASE as *mut u32;
const UART_FLAGS: *const u32 = (UART_BASE + 0x18) as *const u32;
const UART_LINE_CONTROL: *mut u32 = (UART_BASE + 0x2c) as *mut u32;
const UART_CONTROL: *mut u32 = (UART_BASE + 0x30) as *mut u32;
const TX_FIFO_FULL: u32 = 1 << 5;
const WORD_LENGTH_8: u32 = 0b11 << 5;
const TX_ENABLE: u32 = 1 << 8;
const UART_ENABLE: u32 = 1;

// The launcher boots one CPU and startup masks interrupts throughout.
struct Allocator(UnsafeCell<Heap>);
unsafe impl Sync for Allocator {}
unsafe impl GlobalAlloc for Allocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        unsafe { (&mut *self.0.get()).allocate_first_fit(layout) }
            .map_or(core::ptr::null_mut(), NonNull::as_ptr)
    }
    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        unsafe { (&mut *self.0.get()).deallocate(NonNull::new_unchecked(ptr), layout) };
    }
}
#[global_allocator]
static HEAP: Allocator = Allocator(UnsafeCell::new(Heap::empty()));
// Keep the fixed EL0 virtual image range clear of identity-mapped heap addresses.
const HEAP_START: usize = 0x4440_0000;
// The first EL0 arena begins at 0x68000000. Physical heap memory stays
// below that alias in every process's identity map.
const MAPPED_RAM: usize = user::ARENA_BASE - 0x4000_0000;

// Large build VMs have a contiguous privileged heap above the 32-bit EL0 ABI.
const HIGH_HEAP: usize = 1 << 32;
const GIB: usize = 1 << 30;
pub(crate) fn high_heap_end() -> Option<usize> {
    let end = (0x4000_0000 + inspect::ram_size()?).min(32 * GIB) / GIB * GIB;
    (end >= HIGH_HEAP + GIB).then_some(end)
}

fn put(byte: u8) {
    if !process::capture_agent_output(byte) || process::quiet_desktop_worker() { return; }
    #[cfg(feature = "retina")]
    if !graphics::put(byte) { return; }
    #[cfg(feature = "ssh-mux")]
    serial_mux::write(byte);
    #[cfg(not(feature = "ssh-mux"))]
    uart_put(byte);
}

fn uart_put(byte: u8) {
    unsafe {
        while read_volatile(UART_FLAGS) & TX_FIFO_FULL != 0 {
            core::hint::spin_loop();
        }
        write_volatile(UART_DATA, u32::from(byte));
    }
}

fn print(text: &str) {
    for byte in text.bytes() {
        if byte == b'\n' {
            put(b'\r');
        }
        put(byte);
    }
}

fn poll_byte() -> Option<u8> {
    let key = poll_console();
    if key == Some(3) && scheduler::interrupt_reader() { return Some(4); }
    key
}

fn poll_console() -> Option<u8> {
    scheduler::checkpoint();
    if process::agent_noninteractive() || process::quiet_desktop_worker() { return None; }
    if scheduler::background() { return None; }
    #[cfg(feature="split")]
    if scheduler::pane() != scheduler::focus() { return None; }
    #[cfg(feature = "retina")]
    {
        graphics::sample();
        graphics::poll_window();
        if !graphics::tab_input_allowed() { return None; }
        graphics::tab_paste();
        if let Some(key) = graphics::window_input() { return Some(key); }
        let pasted = clipboard::input();
        graphics::apply_resize();
        if let Some(byte) = pasted { return Some(byte); }
        #[cfg(feature = "split")]
        let key = scheduler::input();
        #[cfg(not(feature = "split"))]
        let key = graphics::key();
        match key {
            Some(clipboard::COPY) if graphics::editor_popup() => return Some(3),
            Some(clipboard::COPY) => clipboard::copy(graphics::copied_text()),
            Some(clipboard::PASTE) => clipboard::paste(),
            Some(byte) if graphics::dismiss_edit_menu(byte) => (),
            Some(byte) => return Some(byte),
            None => (),
        }
        if let Some(byte) = clipboard::input() { return Some(byte); }
    }
    #[cfg(feature = "ssh-mux")]
    { serial_mux::poll() }
    #[cfg(not(feature = "ssh-mux"))]
    { unsafe { (&mut *core::ptr::addr_of_mut!(SERIAL_INPUT)).pop_front() }.or_else(uart_poll) }
}

#[cfg(not(feature = "ssh-mux"))]
static mut SERIAL_INPUT: alloc::collections::VecDeque<u8> = alloc::collections::VecDeque::new();

// Peek for ETX during foreground waits. Retain ordinary keyboard and UART
// input for the program that owns the terminal.
fn take_interrupt() -> bool {
    #[cfg(feature = "split")]
    if scheduler::take_key_interrupt() { return true; }
    #[cfg(all(feature = "retina", not(feature = "split")))]
    if graphics::take_key_interrupt() { return true; }
    #[cfg(feature = "ssh-mux")]
    { serial_mux::take_interrupt() }
    #[cfg(not(feature = "ssh-mux"))]
    {
        let queue = unsafe { &mut *core::ptr::addr_of_mut!(SERIAL_INPUT) };
        while queue.len() < 4096 {
            let Some(byte) = uart_poll() else { break };
            if byte == 3 { return true; }
            queue.push_back(byte);
        }
        false
    }
}

fn uart_poll() -> Option<u8> {
    net::idle_poll();
    unsafe { (read_volatile(UART_FLAGS) & (1 << 4) == 0).then(|| read_volatile(UART_DATA) as u8) }
}

fn read_byte() -> u8 {
    if scheduler::background() || process::agent_noninteractive() {return 4;}
    loop {
        if scheduler::stopped() != 0 { return 4; }
        if let Some(byte) = poll_byte() {
            return byte;
        }
        scheduler::sleep_ms(1);
    }
}

fn read_timed_byte(milliseconds: u64) -> Option<u8> {
    let start = monitor::ticks();
    let duration = monitor::seconds_to_ticks(1) * milliseconds / 1000;
    loop {
        if let Some(byte) = poll_byte() {
            return Some(byte);
        }
        if scheduler::stopped() != 0 || monitor::ticks().wrapping_sub(start) >= duration {
            return None;
        }
        scheduler::sleep_ms(1);
    }
}

#[unsafe(no_mangle)]
extern "C" fn console_io(value: u64, operation: u64) -> u64 {
    unsafe extern "C" {
        fn kt_call(request: u64) -> u64;
        fn kt_service_enter();
        fn kt_service_leave();
    }
    let result = if operation == 61 {
        if crate::utility::smp_clock_allowed() != 0 { unsafe { kt_call(value) } } else { u64::MAX }
    } else if matches!(operation, 38 | 43) {
        console_service(value, operation)
    } else {
        unsafe { kt_service_enter(); }
        let result = console_service(value, operation);
        unsafe { kt_service_leave(); }
        result
    };
    match scheduler::stopped() {
        130 => (-4095i64) as u64,
        137 => (-4094i64) as u64,
        _ => result,
    }
}

fn console_service(value: u64, operation: u64) -> u64 {
    #[cfg(feature = "retina")]
    {
        graphics::poll_window();
        if !scheduler::background() && graphics::terminal_closed() { return (-4096i64) as u64; }
    }
    if let Some(result) = user::test_console(value, operation) {
        return result;
    }
    if agent_tools::active() {
        return if operation == 12 { agent_tools::exchange(value) } else { u64::MAX };
    }
    if operation == 57 { scheduler::own_interrupt_key(); return 0; }
    if operation == 55 { return if scheduler::stop_child(value as usize) { 0 } else { u64::MAX }; }
    if operation == 44 {
        return process::python_system();
    }
    if let Some(result) = utility::io(value, operation) { return result; }
    if operation == 18 { return process::exchange(value); }
    if operation == 37 || (33..=35).contains(&operation) || (13..=17).contains(&operation) || (19..=21).contains(&operation) { return foreground::io(value, operation); }
    if operation == 11 { put(value as u8); return value & 255; }
    if operation >= 7 { return u64::MAX; }
    if operation == 6 {
        return git::syscall(value) as u64;
    }
    if operation == 5 {
        return 0;
    }
    if operation == 4 {
        return user::publish_code(value as usize);
    }
    if operation == 3 {
        #[cfg(feature = "retina")]
        graphics::sample();
        return 0;
    }
    if operation == 0 {
        read_byte() as u64
    } else {
        put(value as u8);
        value
    }
}

#[unsafe(no_mangle)]
extern "C" fn kernel_main() -> ! {
    unsafe {
        let ram = inspect::ram_size().unwrap_or(128 * 1024 * 1024).min(MAPPED_RAM);
        let (start, heap_bytes) = high_heap_end().map_or(
            (HEAP_START, (0x4000_0000 + ram).saturating_sub(HEAP_START)),
            |end| (HIGH_HEAP, end - HIGH_HEAP),
        );
        if heap_bytes < 1024 * 1024 { print("Insufficient guest RAM\n"); idle(); }
        (&mut *HEAP.0.get()).init(start as *mut u8, heap_bytes);
        write_volatile(UART_LINE_CONTROL, WORD_LENGTH_8);
        write_volatile(UART_CONTROL, TX_ENABLE | UART_ENABLE | (1 << 9));
    }
    user::init();
    #[cfg(feature = "retina")]
    graphics::init();
    print("LixinOS booted\nKernel ");
    print(inspect::SYSTEM_RELEASE);
    print("\n");
    entropy::init();
    net::init();
    process::run()
}

#[panic_handler]
fn panic(_: &PanicInfo<'_>) -> ! {
    idle()
}

fn idle() -> ! {
    loop {
        // SAFETY: WFI waits for an interrupt and does not access memory.
        unsafe { asm!("wfi", options(nomem, nostack)) };
    }
}
