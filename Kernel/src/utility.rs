// Small stand-alone AArch64 utilities use an ELF64 image with one fixed RX segment.
use crate::{disk, process::Volume, user};
use alloc::{rc::Rc, string::String, vec::Vec};
use core::{
    cell::{RefCell, UnsafeCell},
    slice,
};

pub fn installed(name: &str) -> Option<&'static [u8]> {
    Some(match name {
        "lust" => include_bytes!("../target/user-build/lust.elf"),
        "ls" => include_bytes!("../target/user-build/ls.elf"),
        "echo" => include_bytes!("../target/user-build/core-echo.elf"),
        "cat" => include_bytes!("../target/user-build/core-cat.elf"),
        "grep" => include_bytes!("../target/user-build/core-grep.elf"),
        "wc" => include_bytes!("../target/user-build/core-wc.elf"),
        "ssh" => include_bytes!("../target/user-build/ssh.elf"),
        "desktop" => include_bytes!("../target/user-build/desktop.elf"),
        "netinfo" => include_bytes!("../target/user-build/core-netinfo.elf"),
        "pwd" => include_bytes!("../target/user-build/core-pwd.elf"),
        "clear" => include_bytes!("../target/user-build/core-clear.elf"),
        _ => return None,
    })
}

pub fn segment(file: &[u8]) -> Option<&[u8]> {
    let u16_at = |i| Some(u16::from_le_bytes(file.get(i..i + 2)?.try_into().ok()?));
    let u32_at = |i| Some(u32::from_le_bytes(file.get(i..i + 4)?.try_into().ok()?));
    let u64_at = |i| Some(u64::from_le_bytes(file.get(i..i + 8)?.try_into().ok()?));
    if file.get(..7)? != b"\x7fELF\x02\x01\x01"
        || u16_at(16)? != 2
        || u16_at(18)? != 183
        || u32_at(20)? != 1
        || u64_at(24)? != 0x44000000
        || u16_at(54)? != 56
        || u16_at(56)? != 1
    {
        return None;
    }
    let ph = usize::try_from(u64_at(32)?).ok()?;
    file.get(ph..ph.checked_add(56)?)?;
    if u32_at(ph)? != 1 || u32_at(ph + 4)? != 5 || u64_at(ph + 16)? != 0x44000000 {
        return None;
    }
    let start = usize::try_from(u64_at(ph + 8)?).ok()?;
    let len = usize::try_from(u64_at(ph + 32)?).ok()?;
    if !(4..=disk::FILE_LIMIT).contains(&len) || u64_at(ph + 40)? != len as u64 {
        return None;
    }
    file.get(start..start.checked_add(len)?)
}

struct DirectoryCursor {
    directory: String,
    key: String,
    next_index: u64,
    generation: u64,
}

#[derive(Clone, Default)]
pub(super) struct Streams(Rc<RefCell<StreamData>>);
#[derive(Default)]
struct StreamData {
    output: Option<Vec<u8>>,
    input: Option<alloc::vec::IntoIter<u8>>,
}
impl Streams {
    pub fn new(input: Option<String>, capture: bool) -> Self {
        Self(Rc::new(RefCell::new(StreamData {
            output: capture.then(Vec::new),
            input: input.map(|s| s.into_bytes().into_iter()),
        })))
    }
    pub fn output(&self) -> Result<String, &'static str> {
        String::from_utf8(self.0.borrow_mut().output.take().unwrap_or_default())
            .map_err(|_| "invalid command output")
    }
    pub fn redirected(&self) -> bool {
        let data = self.0.borrow();
        data.input.is_some() || data.output.is_some()
    }
}

pub(super) struct Context {
    connections: crate::net::Connections,
    #[cfg(feature = "retina")]
    pub allow_canvas: bool,
    #[cfg(feature = "retina")]
    pub canvas: Option<crate::graphics::Canvas>,
    #[cfg(feature="retina")]
    pub popup_shell: bool,
    cursor: Option<DirectoryCursor>,
    volume: Rc<RefCell<Volume>>,
    cwd: String,
    pub streams: Streams,
    handles: [Option<alloc::vec::IntoIter<u8>>; 8],
}
struct Slot(UnsafeCell<Option<Context>>);
unsafe impl Sync for Slot {}
static SLOTS: [Slot; user::BANKS] = [const { Slot(UnsafeCell::new(None)) }; user::BANKS];

pub fn active() -> bool { unsafe { (&*SLOTS[user::bank()].0.get()).is_some() } }

impl Context {
    pub fn new(cwd: &str, volume: Rc<RefCell<Volume>>, streams: Streams) -> Self {
        Self {
            #[cfg(feature = "retina")]
            allow_canvas: false,
            #[cfg(feature = "retina")]
            canvas: None,
            #[cfg(feature="retina")]
            popup_shell: false,
            connections: Default::default(), cursor: None, volume, cwd: cwd.into(), streams, handles: core::array::from_fn(|_| None) }
    }
    // Move open handles and the directory cursor back to the caller on yield.
    // Each suspended process therefore retains its own descriptors.
    pub fn enter<T>(self, launch: impl FnOnce() -> T) -> (Self, T) {
        let slot = SLOTS[user::bank()].0.get();
        unsafe { *slot = Some(self); }
        let result = launch();
        (unsafe { (*slot).take().unwrap() }, result)
    }
}

pub fn with_io(
    cwd: &str, volume: Rc<RefCell<Volume>>, input: Option<String>, capture: bool,
    launch: impl FnOnce() -> Result<u8, &'static str>,
) -> Result<(String, u8), &'static str> {
    let streams = Streams::new(input, capture);
    let (_, status) = Context::new(cwd, volume, streams.clone()).enter(launch);
    Ok((streams.output()?, status?))
}

fn terminal_byte(byte: u8) {
    if byte == b'\n' {
        crate::put(b'\r');
    }
    crate::put(byte);
}

pub fn io(value: u64, operation: u64) -> Option<u64> {
    let context = unsafe { (&mut *SLOTS[user::bank()].0.get()).as_mut()? };
    match operation {
        #[cfg(feature = "retina")]
        19..=21 if context.allow_canvas => {
            if context.streams.redirected() { return Some(u64::MAX); }
            if operation == 19 && context.canvas.is_none() {
                context.canvas = crate::graphics::Canvas::open();
            }
            let Some(canvas) = &context.canvas else { return Some(u64::MAX) };
            Some(match operation {
                19 => crate::graphics::canvas_size(),
                20 => crate::graphics::present(&canvas.arena),
                _ => crate::graphics::pointer_event(),
            })
        }
        0 => {
            #[cfg(feature = "split")]
            crate::process::ready_on_read();
            #[cfg(feature="retina")]
            if context.popup_shell && value == 1 {
                return Some(crate::graphics::popup_key());
            }
            let byte = match context.streams.0.borrow_mut().input.as_mut() {
                Some(input) => input.next(),
                None if value == 1 => Some(crate::read_byte()),
                None => match crate::read_byte() {
                    4 => None,
                    13 => Some(10),
                    c => Some(c),
                },
            };
            Some(byte.map_or(256, u64::from))
        }
        1 => {
            if let Some(output) = &mut context.streams.0.borrow_mut().output {
                if output.len() >= disk::FILE_LIMIT {
                    return Some((-2i64) as u64);
                }
                output.push(value as u8);
            } else if value & 256 != 0 {
                crate::put(value as u8);
            } else {
                terminal_byte(value as u8);
            }
            Some(value & 255)
        }
        7 => Some(metadata(context, value) as u64),
        8 => {
            let bytes = unsafe { slice::from_raw_parts(user::utility_mailbox(), 513) };
            let data = bytes
                .iter()
                .position(|b| *b == 0)
                .and_then(|n| core::str::from_utf8(&bytes[..n]).ok())
                .and_then(|name| {
                    let volume = context.volume.borrow();
                    let path = disk::resolve_path(&volume.files, &context.cwd, name).ok()?;
                    volume.files.get(&path)?.clone()
                });
            let fd = context.handles.iter().position(Option::is_none);
            Some(match (data, fd) {
                (Some(bytes), Some(fd)) => {
                    context.handles[fd] = Some(bytes.into_iter());
                    fd as u64 + 3
                }
                _ => u64::MAX,
            })
        }
        9 => {
            let fd = (value >> 32) as usize;
            let count = value as u32 as usize;
            let handle = fd
                .checked_sub(3)
                .and_then(|i| context.handles.get_mut(i))
                .and_then(Option::as_mut);
            let Some(handle) = handle else {
                return Some(u64::MAX);
            };
            if count > 4096 {
                return Some(u64::MAX);
            }
            let buffer = unsafe { slice::from_raw_parts_mut(user::utility_mailbox(), count) };
            let n = count.min(handle.len());
            for byte in &mut buffer[..n] {
                *byte = handle.next().unwrap();
            }
            Some(n as u64)
        }
        10 => {
            let closed = value
                .checked_sub(3)
                .and_then(|fd| context.handles.get_mut(fd as usize))
                .and_then(Option::take);
            Some(if closed.is_some() { 0 } else { u64::MAX })
        }
        11 => {
            terminal_byte(value as u8);
            Some(value & 255)
        }
        25..=28 => {
            let mail = unsafe { slice::from_raw_parts_mut(user::utility_mailbox(), 4096) };
            Some(context.connections.call(operation, value, mail) as u64)
        }
        29 => {
            let Some(text) = crate::net::info() else { return Some(u64::MAX) };
            let mail = unsafe { slice::from_raw_parts_mut(user::utility_mailbox(), 4096) };
            mail[..text.len()].copy_from_slice(text.as_bytes());
            Some(text.len() as u64)
        }
        30 => {
            if value > 4096 { return Some(u64::MAX); }
            let mail = unsafe { slice::from_raw_parts_mut(user::utility_mailbox(), value as usize) };
            Some(if crate::entropy::fill(mail).is_some() { 0 } else { u64::MAX })
        }
        24 if value == 1 => Some(u64::from(!context.streams.redirected())),
        24 => Some(u64::from(context.streams.0.borrow().output.is_none())),
        // SVC 22 atomically stores a file from a sparse-buffer descriptor or
        // legacy mailbox length. SVC 23 returns the resolved path.
        22 | 23 | 31 => {
            let mail = unsafe { slice::from_raw_parts_mut(user::utility_mailbox(), 513) };
            let path = mail.iter().position(|&b| b == 0)
                .and_then(|n| core::str::from_utf8(&mail[..n]).ok())
                .and_then(|s| disk::resolve_path(&context.volume.borrow().files, &context.cwd, s).ok());
            let Some(path) = path else { return Some(u64::MAX) };
            if operation == 23 {
                mail[..path.len()].copy_from_slice(path.as_bytes());
                mail[path.len()] = 0;
                return Some(0);
            }

            let mut volume = context.volume.borrow_mut();
            // Exclusive creation for the first accepted SSH host fingerprint.
            if operation == 31 && volume.files.contains_key(&path) { return Some(u64::MAX); }
            if volume.files.get(&path) == Some(&None) { return Some(u64::MAX); }
            let bytes = if value >> 32 != 0 {
                let Some(bytes) = user::copy_user_file(value) else { return Some(u64::MAX) };
                bytes
            } else if value <= user::SCRIPT_CAPACITY as u64 {
                // Original ABI: length only, bytes in the launch mailbox.
                unsafe { slice::from_raw_parts(user::git_mailbox(), value as usize) }.to_vec()
            } else {
                return Some(u64::MAX);
            };
            Some(if volume.commit(path, Some(Some(bytes)), None).is_ok() { 0 } else { u64::MAX })
        }
        _ => None,
    }
}

fn metadata(context: &mut Context, index: u64) -> i64 {
    // Both fields are bounded, within a dedicated page of the active EL0 bank.
    let mailbox = unsafe { slice::from_raw_parts_mut(user::utility_mailbox(), 1026) };
    let (request, response) = mailbox.split_at_mut(513);
    let Some(end) = request.iter().position(|&b| b == 0) else {
        return -5;
    };
    let Ok(name) = core::str::from_utf8(&request[..end]) else {
        return -5;
    };
    let volume = context.volume.borrow();
    let path = match disk::resolve_path(&volume.files, &context.cwd, name) {
        Ok(path) => path,
        Err("not a directory") => return -4,
        Err(_) => return -5,
    };
    let Some(entry) = volume.files.get(&path) else {
        return -3;
    };
    if index == 0 {
        return entry.as_ref().map_or(-2, |data| data.len() as i64);
    }
    if entry.is_some() {
        return -4;
    }
    let generation = volume.disk.snapshot_status().1;
    let after = context.cursor.as_ref().filter(|c|
        c.directory == path && c.next_index == index && c.generation == generation
    ).map(|c| c.key.as_str());
    let skip = if after.is_some() { 0 } else { (index - 1) as usize };
    let Some((name, entry)) = disk::children_after(&volume.files, &path, after).nth(skip) else {
        return -1;
    };
    context.cursor = Some(DirectoryCursor {
        key: alloc::format!("{}/{name}", path.trim_end_matches('/')),
        directory: path, next_index: index + 1, generation,
    });
    if name.len() >= response.len() {
        return -5;
    }
    response[..name.len()].copy_from_slice(name.as_bytes());
    response[name.len()] = 0;
    entry.as_ref().map_or(-2, |data| data.len() as i64)
}
