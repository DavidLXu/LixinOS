unsafe extern "C" {
    fn lx_macro_pipe(fd: i32, data: *mut u8, len: usize, writing: i32) -> i32;
}

pub(super) fn read(fd: i32, buf: &mut [u8]) -> i32 {
    // The guest adapter writes at most buf.len() bytes during this call.
    unsafe { lx_macro_pipe(fd, buf.as_mut_ptr(), buf.len(), 0) }
}

pub(super) fn write(fd: i32, buf: &[u8]) -> i32 {
    // In write mode the adapter only reads from this slice.
    unsafe { lx_macro_pipe(fd, buf.as_ptr().cast_mut(), buf.len(), 1) }
}
