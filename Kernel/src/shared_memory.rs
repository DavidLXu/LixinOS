//! File descriptions used for anonymous cross-process shared memory.
use alloc::alloc::{alloc_zeroed, dealloc, Layout};
use alloc::rc::Rc;
use core::{cell::RefCell, ptr::NonNull};

pub(crate) const PAGE: usize = 4096;
pub(crate) const LIMIT: usize = 16 << 20;
pub(crate) const MAPS: usize = 16;
pub(crate) const RIGHTS: usize = 8;
pub(crate) const MARKERS: usize = 16;
pub(crate) const CONTROL: usize = 4096 - (2 + RIGHTS) * 4;

pub(crate) type Shared = Rc<RefCell<File>>;

struct Pages {
    ptr: NonNull<u8>,
    layout: Layout,
}

impl Drop for Pages {
    fn drop(&mut self) {
        unsafe { dealloc(self.ptr.as_ptr(), self.layout) };
    }
}

#[derive(Default)]
pub(crate) struct File {
    pages: Option<Pages>,
    pub len: usize,
    pub position: usize,
    pub mappings: usize,
}

impl File {
    pub fn resize(&mut self, size: usize) -> i64 {
        if size > LIMIT { return -27; }
        if size == self.len { return 0; }
        if self.mappings != 0 { return -16; }
        if size == 0 {
            self.pages = None;
        } else {
            let layout = Layout::from_size_align(size.next_multiple_of(PAGE), PAGE).unwrap();
            let Some(ptr) = NonNull::new(unsafe { alloc_zeroed(layout) }) else { return -12; };
            if let Some(old) = &self.pages {
                unsafe { core::ptr::copy_nonoverlapping(old.ptr.as_ptr(), ptr.as_ptr(), self.len.min(size)); }
            }
            self.pages = Some(Pages { ptr, layout });
        }
        self.len = size;
        0
    }

    pub fn physical(&self) -> usize {
        self.pages.as_ref().map_or(0, |p| p.ptr.as_ptr() as usize)
    }

    pub fn transfer(&mut self, bytes: &mut [u8], write: bool) -> i64 {
        if bytes.is_empty() { return 0; }
        if write && self.position + bytes.len() > self.len {
            let status = self.resize(self.position + bytes.len());
            if status < 0 { return status; }
        }
        let count = bytes.len().min(self.len.saturating_sub(self.position));
        if count != 0 {
            let memory = (self.physical() + self.position) as *mut u8;
            unsafe {
                if write { core::ptr::copy_nonoverlapping(bytes.as_ptr(), memory, count); }
                else { core::ptr::copy_nonoverlapping(memory, bytes.as_mut_ptr(), count); }
            }
            self.position += count;
        }
        count as i64
    }

    pub fn seek(&mut self, distance: i64, whence: u32) -> i64 {
        let base = match whence { 0 => 0, 1 => self.position, 2 => self.len, _ => return -22 };
        match (base as i64).checked_add(distance) {
            Some(next) if (0..=LIMIT as i64).contains(&next) => {
                self.position = next as usize;
                next
            }
            _ => -22,
        }
    }
}
