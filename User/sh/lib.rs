#![no_std]
extern crate alloc;
mod abi;
#[path = "../foreground/keys.rs"]
mod keys;
#[path = "main.rs"]
mod shell;
use core::{
    alloc::{GlobalAlloc, Layout},
    panic::PanicInfo,
};

unsafe extern "C" {
    fn memalign(align: usize, n: usize) -> *mut u8;
    fn free(p: *mut u8);
    fn read_raw() -> i32;
    fn read_timed(ms: u32) -> i32;
    fn screen_info() -> u64;
    fn screen_write(p: *const u8, n: usize);

    fn _exit(code: i32) -> !;
}
struct Allocator;
unsafe impl GlobalAlloc for Allocator {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        unsafe { memalign(l.align().max(16), l.size()) }
    }
    unsafe fn dealloc(&self, p: *mut u8, _: Layout) {
        unsafe { free(p) }
    }
}
#[global_allocator]
static HEAP: Allocator = Allocator;
#[panic_handler]
fn panic(_: &PanicInfo) -> ! {
    unsafe { _exit(1) }
}
fn read_byte() -> u8 {
    unsafe { read_raw() as u8 }
}
fn geometry() -> u64 {
    unsafe { screen_info() }
}
#[unsafe(no_mangle)]
pub extern "C" fn terminal_key() -> i32 {
    keys::decode(read_byte(), || {
        u8::try_from(unsafe { read_timed(100) }).ok()
    })
}
fn print(s: &str) {
    for part in s.split_inclusive('\n') {
        let text = part.trim_end_matches('\n');
        unsafe {
            screen_write(text.as_ptr(), text.len());
        }
        if part.ends_with('\n') {
            unsafe {
                screen_write(b"\r\n".as_ptr(), 2);
            }
        }
    }
}
