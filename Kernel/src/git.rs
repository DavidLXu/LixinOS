//! Each foreground git command publishes its metadata changes in one disk snapshot.
use crate::{
    disk::{ENTRY_LIMIT, Files},
    process::Volume,
    user,
};
use alloc::{
    collections::{BTreeMap, BTreeSet},
    format,
    rc::Rc,
    string::String,
};
use core::cell::{RefCell, UnsafeCell};

type Entry = Option<crate::disk::FileData>;
type Changes = BTreeMap<String, Option<Entry>>;
struct Session {
    volume: Rc<RefCell<Volume>>,
    heap: user::UserArena,
    metadata: String,
    generation: u64,
    changes: Changes,
}
struct Slots(UnsafeCell<[Option<Session>; user::BANKS]>);
// Single CPU. Each pane has its own slot, and a syscall never yields.
unsafe impl Sync for Slots {}
static SLOTS: Slots = Slots(UnsafeCell::new([const { None }; user::BANKS]));
pub fn run(volume: Rc<RefCell<Volume>>, root: &str, args: &[String]) -> Result<(), &'static str> {
    let mut packed = args.join("\0");
    packed.push('\0');
    if packed.len() >= 1024 {
        return Err("git arguments too long");
    }
    let generation = volume.borrow().disk.revision();
    let bank = user::bank();
    unsafe {
        (*SLOTS.0.get())[bank] = Some(Session {
            heap: user::UserArena::for_browser_heap(),
            volume,
            metadata: format!("{}/.git", root.trim_end_matches('/')),
            generation,
            changes: Changes::new(),
        });
    }
    let result = user::run(
        include_bytes!("../target/user-build/git.bin"),
        Some((root, &packed)),
    );
    let session = unsafe { (*SLOTS.0.get())[bank].take().unwrap() };
    result?;
    session.publish()?;
    let message = unsafe { &*user::git_mailbox().cast::<Message>() };
    let line = message
        .bytes
        .get(..message.length as usize)
        .ok_or("invalid git confirmation")?;
    crate::print(core::str::from_utf8(line).map_err(|_| "invalid git confirmation")?);
    Ok(())
}
impl Session {
    fn publish(self) -> Result<(), &'static str> {
        let mut borrowed = self.volume.borrow_mut();
        let volume = &mut *borrowed;
        if volume.disk.revision() != self.generation {
            return Err("filesystem changed during git command; retry");
        }
        if self.changes.is_empty() {
            return Ok(());
        }
        let mut undo = Changes::new();
        for (p, v) in self.changes {
            let old = match v {
                Some(e) => volume.files.insert(p.clone(), e),
                None => volume.files.remove(&p),
            };
            undo.insert(p, old);
        }
        let result = if volume.files.len() > ENTRY_LIMIT {
            Err("filesystem full")
        } else {
            volume.save_changes(undo.iter().map(|(path, old)| (path.as_str(), old.is_some())), &crate::disk::Times::new())
        };
        if result.is_err() {
            for (p, v) in undo {
                match v {
                    Some(e) => {
                        volume.files.insert(p, e);
                    }
                    None => {
                        volume.files.remove(&p);
                    }
                }
            }
        }
        result
    }
}
#[repr(C)]
struct Message {
    length: i32,
    number: i32,
    path: [u8; 513],
    other: [u8; 513],
    bytes: [u8; 16384], // Matches FILE_CAP in User/git/platform.c.
}
fn path(bytes: &[u8]) -> Result<&str, i64> {
    let end = bytes.iter().position(|b| *b == 0).ok_or(22i64)?;
    let s = core::str::from_utf8(&bytes[..end]).map_err(|_| 22i64)?;
    if s != "/" && (!s.starts_with('/') || s[1..].split('/').any(|x| matches!(x, "" | "." | "..")))
    {
        return Err(22);
    }
    Ok(s)
}
fn lookup<'a>(files: &'a Files, changes: &'a Changes, key: &str) -> Option<&'a Entry> {
    changes
        .get(key)
        .map_or_else(|| files.get(key), Option::as_ref)
}
fn children<'a>(files: &'a Files, changes: &'a Changes, p: &str) -> BTreeSet<&'a str> {
    let prefix = format!("{}/", p.trim_end_matches('/'));
    files
        .keys()
        .chain(changes.keys())
        .filter(|k| lookup(files, changes, k).is_some())
        .filter_map(|k| k.strip_prefix(&prefix))
        .filter(|k| !k.is_empty() && !k.contains('/'))
        .collect()
}
pub fn syscall(op: u64) -> i64 {
    let Some(s) = (unsafe { (*SLOTS.0.get())[user::bank()].as_mut() }) else {
        return -13;
    };
    let m = unsafe { &mut *user::git_mailbox().cast::<Message>() };
    operation(s, m, op).unwrap_or_else(|e| -e)
}
fn operation(s: &mut Session, m: &mut Message, op: u64) -> Result<i64, i64> {
    if op == 7 {
        return Ok(unsafe { core::ptr::read_volatile(0x0901_0000 as *const u32) } as i64);
    }
    if op == 10 {
        s.heap.grow(m.length as usize).map_err(|_| 12i64)?;
        return Ok(user::WEB_HEAP_BASE as i64);
    }
    let p = path(&m.path)?;
    let volume = s.volume.borrow();
    let files = &volume.files;
    let changes = &mut s.changes;
    let entry = lookup(files, changes, p);
    let writable = |k: &str| {
        k == s.metadata
            || k.strip_prefix(&s.metadata)
                .is_some_and(|suffix| suffix.starts_with('/'))
    };
    let parent = |key: &str| {
        let dir = key.rsplit_once('/').unwrap().0;
        lookup(files, changes, if dir.is_empty() { "/" } else { dir }) == Some(&None)
    };
    if ((3..=6).contains(&op) || op == 9) && !writable(p) {
        return Err(13);
    }
    match op {
        0 => {
            let entry = entry.ok_or(2i64)?;
            m.length = entry.as_ref().map_or(-1, |b| b.len() as i32);
            m.number = volume.disk.revision() as i32 + 1;
            Ok(0)
        }
        1 => {
            if entry.ok_or(2i64)?.is_some() {
                return Err(20);
            }
            let names = children(files, changes, p);
            if let Some(name) = names.iter().nth(m.number as usize) {
                m.other[..name.len()].copy_from_slice(name.as_bytes());
                m.other[name.len()] = 0;
                Ok(0)
            } else {
                Ok(1)
            }
        }
        2 => {
            let data = entry.ok_or(2i64)?.as_ref().ok_or(21i64)?;
            m.bytes.get_mut(..data.len()).ok_or(27i64)?.copy_from_slice(data);
            Ok(data.len() as i64)
        }
        8 | 9 => {
            let offset = usize::try_from(m.number).map_err(|_| 22i64)?;
            let size = m.length as usize;
            if size > m.bytes.len() { return Err(22); }
            let data = entry.ok_or(2i64)?.as_ref().ok_or(21i64)?;
            if op == 8 {
                let n = size.min(data.len().saturating_sub(offset));
                if n != 0 { m.bytes[..n].copy_from_slice(&data[offset..offset+n]); }
                return Ok(n as i64);
            }
            let end = offset.checked_add(size).ok_or(27i64)?;
            if end > volume.disk.file_limit() { return Err(27); }
            if !changes.contains_key(p) {
                changes.insert(p.into(), Some(Some(data.clone())));
            }
            let target = changes.get_mut(p).and_then(Option::as_mut)
                .and_then(Option::as_mut).ok_or(21i64)?
                .as_mut_vec().map_err(|_| 5i64)?;
            if end > target.len() {
                target.try_reserve(end-target.len()).map_err(|_| 12i64)?;
                target.resize(end, 0);
            }
            target[offset..end].copy_from_slice(&m.bytes[..size]);
            Ok(0)
        }
        3 | 4 => {
            if op == 4 && entry.is_some() {
                return Err(17);
            }
            if entry == Some(&None) {
                return Err(21);
            }
            if !parent(p) {
                return Err(2);
            }
            let value = if op == 4 {
                None
            } else {
                Some(crate::disk::FileData::from_vec(m.bytes.get(..m.length as usize).ok_or(27i64)?.to_vec()))
            };
            changes.insert(p.into(), Some(value));
            Ok(0)
        }
        5 => {
            let dir = entry.ok_or(2i64)?.is_none();
            if dir != (m.number != 0) {
                return Err(if dir { 21 } else { 20 });
            }
            if dir && !children(files, changes, p).is_empty() {
                return Err(90);
            }
            changes.insert(p.into(), None);
            Ok(0)
        }
        6 => {
            let dest = path(&m.other)?;
            if !writable(dest) {
                return Err(13);
            }
            if !parent(dest) {
                return Err(2);
            }
            if lookup(files, changes, dest) == Some(&None) {
                return Err(21);
            }
            let content = entry.ok_or(2i64)?.as_ref().ok_or(21i64)?.clone();
            changes.insert(p.into(), None);
            changes.insert(dest.into(), Some(Some(content)));
            Ok(0)
        }
        _ => Err(22),
    }
}
