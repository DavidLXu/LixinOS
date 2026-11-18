//! Launch foreground applications with a per-pane grant for one file, or read-only metrics.
use crate::{disk, monitor, process::Volume, user};
use alloc::{rc::Rc, string::String, vec::Vec};
use core::cell::{RefCell, UnsafeCell};

pub const IMAGE: &[u8] = include_bytes!("../target/user-build/foreground.bin");

struct FileGrant {
    path: String,
    expected: Option<Vec<u8>>,
    arena: user::UserArena,
}
struct Context {
    volume: Rc<RefCell<Volume>>,
    file: Option<FileGrant>,
    started: u64,
    #[cfg(feature = "retina")]
    canvas: bool,
}
struct Slot(UnsafeCell<Option<Context>>);
unsafe impl Sync for Slot {}
static SLOTS: [Slot; user::BANKS] = [const { Slot(UnsafeCell::new(None)) }; user::BANKS];

pub fn run(
    name: &str,
    path: Option<String>,
    volume: Rc<RefCell<Volume>>,
    started: u64,
) -> Result<(), &'static str> {
    let file = if let Some(path) = path {
        let borrowed = volume.borrow();
        let expected = borrowed
            .files
            .get(&path)
            .map(|v| v.clone().ok_or("is a directory"))
            .transpose()?;
        let bytes = expected.as_deref().unwrap_or_default();
        if core::str::from_utf8(bytes).is_err() || bytes.contains(&0) {
            return Err("binary file");
        }
        let mut arena = user::UserArena::new();
        arena.grow(disk::FILE_LIMIT + 1)?;
        unsafe {
            core::ptr::copy_nonoverlapping(
                bytes.as_ptr(),
                user::ARENA_BASE as *mut u8,
                bytes.len(),
            );
        }
        Some(FileGrant {
            path,
            expected,
            arena,
        })
    } else {
        None
    };
    let length = file
        .as_ref()
        .and_then(|f| f.expected.as_ref())
        .map_or(0, Vec::len);
    let args = alloc::format!(
        "{}\0{}",
        name,
        file.as_ref().map_or("", |f| f.path.as_str())
    );
    let source = alloc::format!("{length}");
    let slot = SLOTS[user::bank()].0.get();
    unsafe {
        *slot = Some(Context {
            volume,
            file,
            started,
            #[cfg(feature = "retina")]
            canvas: name == "paint",
        });
    }
    #[cfg(feature = "retina")]
    if name == "paint" {
        crate::graphics::begin_canvas();
    }
    let result = user::run(IMAGE, Some((&args, &source)));
    unsafe {
        (*slot).take();
    }
    result
}

pub fn io(value: u64, op: u64) -> u64 {
    if op == 13 {
        #[cfg(feature = "retina")]
        let (cols, rows) = (crate::graphics::columns(), crate::graphics::rows());
        #[cfg(not(feature = "retina"))]
        let (cols, rows) = (80, 24);
        return ((cfg!(feature = "window-console") as u64) << 32) | ((rows as u64) << 16) | cols as u64;
    }
    if op == 14 {
        if !crate::utility::active() && !crate::process::active() && unsafe { (&*SLOTS[user::bank()].0.get()).is_none() } {
            return u64::MAX;
        }
        let start = monitor::ticks();
        let duration = monitor::seconds_to_ticks(1) * value.min(1000) / 1000;
        #[cfg(feature = "retina")]
        crate::graphics::waiting(true);
        let result = loop {
            if let Some(b) = crate::poll_byte() {
                break u64::from(b);
            }
            if monitor::ticks().wrapping_sub(start) >= duration {
                break 256;
            }
        };
        #[cfg(feature = "retina")]
        crate::graphics::waiting(false);
        return result;
    }
    let Some(ctx) = (unsafe { (&mut *SLOTS[user::bank()].0.get()).as_mut() }) else {
        return u64::MAX;
    };
    #[cfg(feature = "retina")]
    if (19..=21).contains(&op) {
        if !ctx.canvas {
            return u64::MAX;
        }
        return match op {
            19 => crate::graphics::canvas_size(),
            20 => {
                let Some(file) = &ctx.file else {
                    return u64::MAX;
                };
                crate::graphics::present(&file.arena)
            }
            _ => crate::graphics::pointer_event(),
        };
    }
    if op == 16 {
        let (files, dirs, bytes) = disk::file_counts(&ctx.volume.borrow().files);
        let (used, free) = monitor::heap_usage();
        let stats = [
            (monitor::ticks() - ctx.started) / monitor::seconds_to_ticks(1),
            used as u64,
            free as u64,
            files as u64,
            dirs as u64,
            bytes as u64,
            if user::bank() == 0 { 16 } else { 32 },
            crate::inspect::ram_size().unwrap_or(0) as u64,
        ];
        unsafe {
            core::ptr::copy_nonoverlapping(stats.as_ptr(), user::git_mailbox().cast(), stats.len());
        }
        return 0;
    }
    let Some(grant) = ctx.file.as_mut() else {
        return u64::MAX;
    };
    if op == 17 {
        return if grant.arena.grow(value as usize).is_ok() {
            0
        } else {
            u64::MAX
        };
    }
    if op != 15 || value > disk::FILE_LIMIT as u64 {
        return 1;
    }
    let mut volume = ctx.volume.borrow_mut();
    if volume.files.get(&grant.path).map(Option::as_ref) != grant.expected.as_ref().map(Some) {
        return 2;
    }
    let bytes =
        unsafe { core::slice::from_raw_parts(user::ARENA_BASE as *const u8, value as usize) }
            .to_vec();
    match volume.commit(grant.path.clone(), Some(Some(bytes.clone())), None) {
        Ok(()) => {
            grant.expected = Some(bytes);
            0
        }
        Err("filesystem full") => 3,
        Err(_) => 4,
    }
}
