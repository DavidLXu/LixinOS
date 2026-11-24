//! Launch foreground applications with a per-pane grant for one file, or read-only metrics.
const EDITOR_LIMIT: usize = 1024 * 1024;

use crate::{disk, monitor, process::Volume, scheduler, user};
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
    heap: user::UserArena,
    #[cfg(feature = "retina")]
    canvas: bool,
    #[cfg(feature = "retina")]
    fixed_geometry: bool,
}
struct Slot(UnsafeCell<Option<Context>>);
unsafe impl Sync for Slot {}
static SLOTS: [Slot; user::BANKS] = [const { Slot(UnsafeCell::new(None)) }; user::BANKS];

#[cfg(feature = "retina")]
pub(crate) fn editing() -> bool {
    unsafe { (&*SLOTS[user::bank()].0.get()).as_ref() }
        .is_some_and(|ctx| ctx.file.is_some() && !ctx.canvas)
}

#[cfg(feature = "retina")]
pub(crate) fn canvas_active() -> bool {
    unsafe { (&*SLOTS[user::bank()].0.get()).as_ref() }.is_some_and(|ctx| ctx.canvas)
}

#[cfg(feature = "retina")]
pub(crate) fn blocks_resize() -> bool {
    SLOTS.iter().any(|slot| unsafe { (&*slot.0.get()).as_ref() }
        .is_some_and(|ctx| ctx.fixed_geometry))
}

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
            .map(|v| v.as_ref().map(disk::FileData::to_vec).ok_or("is a directory"))
            .transpose()?;
        let bytes = expected.as_deref().unwrap_or_default();
        if bytes.len() > EDITOR_LIMIT { return Err("file too large for editor"); }
        if core::str::from_utf8(bytes).is_err() || bytes.contains(&0) {
            return Err("binary file");
        }
        let mut arena = user::UserArena::new();
        arena.grow(EDITOR_LIMIT + 1)?;
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
    let native = volume.borrow().files.get(&alloc::format!("/libexec/{name}")).and_then(Option::as_ref).map(disk::FileData::to_vec);
    let slot = SLOTS[user::bank()].0.get();
    unsafe {
        *slot = Some(Context {
            volume,
            #[cfg(feature = "retina")]
            fixed_geometry: file.is_some() && (name != "nano" || native.is_some()),
            file,
            started,
            heap: user::UserArena::for_browser_heap(),
            #[cfg(feature = "retina")]
            canvas: name == "paint",
        });
    }
    #[cfg(feature = "retina")]
    if name == "paint" {
        crate::graphics::begin_canvas();
    }
    let result = if let Some(image) = native {
        let packed = alloc::format!("{args}\0{length}\0");
        match user::start_saved(&image, 3, &packed, 0) {
            Ok(0) => Ok(()),
            Ok(_) => Err("foreground application failed"),
            Err(e) => Err(e),
        }
    } else {
        user::run(IMAGE, Some((&args, &source)))
    };
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
        #[cfg(feature = "retina")]
        let editor = crate::graphics::editor_popup() as u64;
        #[cfg(not(feature = "retina"))]
        let editor = 0u64;
        return (editor << 33) | ((cfg!(feature = "window-console") as u64) << 32) | ((rows as u64) << 16) | cols as u64;
    }
    if op == 14 {
        if crate::process::agent_noninteractive() { return 4; }
        if !crate::utility::active() && !crate::process::active() && unsafe { (&*SLOTS[user::bank()].0.get()).is_none() } {
            return u64::MAX;
        }
        return crate::read_timed_byte(value.min(1000)).map_or(256, u64::from);
    }
    #[cfg(feature = "retina")]
    if op == 33 && editing() && crate::graphics::editor_popup() {
        return crate::graphics::editor_event();
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
            _ => crate::graphics::pointer_event(0),
        };
    }
    if op == 37 {
        return if ctx.heap.grow(value as usize).is_ok() { user::WEB_HEAP_BASE as u64 } else { u64::MAX };
    }
    if op == 16 && value == 1 {
        // Header followed by the same per-process records as sysinfo 29/9.
        let words = unsafe { core::slice::from_raw_parts_mut(user::git_mailbox().cast::<u64>(), 512) };
        words[0] = scheduler::uptime_ticks();
        words[1] = monitor::seconds_to_ticks(1);
        words[2] = monitor::heap_usage().1 as u64;
        let cores = unsafe { scheduler::smp_cpus() };
        words[3] = u64::from(cores);
        for cpu in 0..2 {
            words[4 + cpu as usize] = if cpu < cores { unsafe { scheduler::smp_idle_ticks(cpu) } } else { 0 };
        }
        return (48 + scheduler::resource_snapshot(&mut words[6..])) as u64;
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
            if crate::scheduler::pane() == 0 { 16 } else { 32 },
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
    #[cfg(feature = "retina")]
    if (34..=35).contains(&op) && crate::graphics::editor_popup() {
        if op == 35 {
            let bytes = crate::clipboard::local();
            if !grant.arena.contains(user::ARENA_BASE, bytes.len()) {
                return u64::MAX;
            }
            unsafe {
                core::ptr::copy_nonoverlapping(bytes.as_ptr(), user::ARENA_BASE as *mut u8, bytes.len());
            }
            return bytes.len() as u64;
        }
        if value > EDITOR_LIMIT as u64 || !grant.arena.contains(user::ARENA_BASE, value as usize) {
            return u64::MAX;
        }
        let bytes = unsafe { core::slice::from_raw_parts(user::ARENA_BASE as *const u8, value as usize) };
        crate::clipboard::copy_document(bytes.to_vec());
        return 0;
    }
    if op == 17 {
        return if grant.arena.grow(value as usize).is_ok() {
            0
        } else {
            u64::MAX
        };
    }
    if op != 15 || value > EDITOR_LIMIT as u64 {
        return 1;
    }
    let mut volume = ctx.volume.borrow_mut();
    if volume.files.get(&grant.path).map(Option::as_deref) != grant.expected.as_deref().map(Some) {
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
