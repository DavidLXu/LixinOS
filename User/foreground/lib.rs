#![no_std]
extern crate alloc;
mod keys;
mod nano;
use core::{
    alloc::{GlobalAlloc, Layout},
    ffi::{CStr, c_char},
    panic::PanicInfo,
    slice, str,
};

unsafe extern "C" {
    fn memalign(align: usize, n: usize) -> *mut u8;
    fn free(p: *mut u8);
    fn read_raw() -> i32;
    fn read_timed(ms: u32) -> i32;
    fn screen_info() -> u64;
    fn screen_write(p: *const u8, n: usize);
    fn save_file(p: *const u8, n: usize) -> i32;
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
#[unsafe(export_name = "nano")]
pub unsafe extern "C" fn launch(name: *const c_char, text: *const u8, len: usize) {
    let name = unsafe { CStr::from_ptr(name) }.to_str().unwrap();
    let source = str::from_utf8(unsafe { slice::from_raw_parts(text, len) }).unwrap();
    nano::edit(name, source, 1024 * 1024, |contents| {
        match unsafe { save_file(contents.as_ptr(), contents.len()) } {
            0 => Ok(()),
            1 => Err("File size limit"),
            2 => Err("file changed in other pane"),
            3 => Err("filesystem full"),
            _ => Err("disk write failed"),
        }
    });
}
