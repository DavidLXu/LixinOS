#![no_std]
#![no_main]

extern crate alloc;
mod agent;
mod disk;
mod editor;
#[cfg(feature = "retina")]
mod graphics;
mod inspect;
mod keyboard;
mod monitor;
#[cfg(feature = "split")]
mod panes;
mod shell;
mod user;

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
const HEAP_BYTES: usize = 8 * 1024 * 1024;
static mut MEMORY: [u8; HEAP_BYTES] = [0; HEAP_BYTES];

fn put(byte: u8) {
    #[cfg(feature = "retina")]
    graphics::put(byte);
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
    #[cfg(feature = "retina")]
    graphics::sample();
    #[cfg(feature = "split")]
    if let Some(byte) = panes::input() {
        return Some(byte);
    }
    #[cfg(all(feature = "retina", not(feature = "split")))]
    if let Some(byte) = graphics::key() {
        return Some(byte);
    }
    unsafe { (read_volatile(UART_FLAGS) & (1 << 4) == 0).then(|| read_volatile(UART_DATA) as u8) }
}

fn read_byte() -> u8 {
    #[cfg(feature = "retina")]
    graphics::waiting(true);
    loop {
        if let Some(byte) = poll_byte() {
            #[cfg(feature = "retina")]
            graphics::waiting(false);
            return byte;
        }
        core::hint::spin_loop();
    }
}

#[unsafe(no_mangle)]
extern "C" fn console_io(value: u64, operation: u64) -> u64 {
    if let Some(result) = user::test_console(value, operation) {
        return result;
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
        (&mut *HEAP.0.get()).init(core::ptr::addr_of_mut!(MEMORY).cast(), HEAP_BYTES);
        write_volatile(UART_LINE_CONTROL, WORD_LENGTH_8);
        write_volatile(UART_CONTROL, TX_ENABLE | UART_ENABLE | (1 << 9));
    }
    #[cfg(feature = "retina")]
    graphics::init();
    print(concat!("LixinOS booted\nVersion ", env!("CARGO_PKG_VERSION"), "\n"));
    user::init();
    shell::run()
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
