//! Clipboard and window-size messages for the graphical console.
use crate::{disk::Dma, monitor};
use alloc::vec::Vec;
use core::{cell::UnsafeCell, fmt::Write, ptr::NonNull};
use virtio_drivers::{
    device::console::VirtIOConsole,
    transport::{DeviceType, Transport, mmio::MmioTransport},
};

const LIMIT: usize = 4096;
pub const COPY: u8 = 0xfd;
pub const PASTE: u8 = 0xfc;

#[derive(Clone, Copy)]
struct Target {
    pid: usize,
    editor: bool,
    multiline: bool,
    block: bool,
}

struct Paste {
    target: Target,
    bytes: alloc::vec::IntoIter<u8>,
}

struct Request {
    id: u64,
    target: Target,
    deadline: u64,
}

struct Clipboard {
    host: Option<VirtIOConsole<Dma, MmioTransport<'static>>>,
    local: Vec<u8>,
    reply: Vec<u8>,
    overflow: bool,
    sequence: u64,
    request: Option<Request>,
    ready: Option<Paste>,
}

struct Slot(UnsafeCell<Clipboard>);
// Accessed only from the graphical input path on the single vCPU.
unsafe impl Sync for Slot {}
static SLOT: Slot = Slot(UnsafeCell::new(Clipboard {
    host: None,
    local: Vec::new(),
    reply: Vec::new(),
    overflow: false,
    sequence: 0,
    request: None,
    ready: None,
}));

pub fn init() {
    let transport =
        unsafe { MmioTransport::new(NonNull::new(0x0a00_0c00 as *mut _).unwrap(), 512) };
    if let Ok(transport) = transport {
        if transport.device_type() == DeviceType::Console {
            unsafe { (*SLOT.0.get()).host = VirtIOConsole::new(transport).ok() };
        }
    }
}

pub fn local() -> Vec<u8> {
    unsafe { (*SLOT.0.get()).local.clone() }
}

pub fn copy(mut bytes: Vec<u8>) {
    bytes.truncate(LIMIT);
    copy_document(bytes);
}

pub fn copy_document(bytes: Vec<u8>) {
    if bytes.is_empty() {
        return;
    }
    let clipboard = unsafe { &mut *SLOT.0.get() };
    clipboard.local = bytes;
    if clipboard.local.len() > LIMIT {
        return;
    }
    if let Some(host) = &mut clipboard.host {
        let mut frame = alloc::string::String::from("C\t");
        for byte in &clipboard.local {
            write!(frame, "{byte:02x}").unwrap();
        }
        frame.push('\n');
        let _ = host.send_bytes(frame.as_bytes());
    }
}

fn prepare(bytes: &[u8], target: Target) -> Paste {
    let bytes = if target.block { &bytes[..bytes.len().min(LIMIT)] } else { bytes };
    let mut normalized = Vec::with_capacity(bytes.len());
    let mut previous_cr = false;
    for &byte in bytes {
        match byte {
            b'\n' if previous_cr => (),
            b'\r' | b'\n' => normalized.push(if target.block { b'\n' } else if target.multiline { b'\r' } else { b' ' }),
            b'\t' => normalized.push(if target.block || target.multiline { b'\t' } else { b' ' }),
            32..=126 => normalized.push(byte),
            128..=255 if target.block => normalized.push(byte),
            _ => (),
        }
        previous_cr = byte == b'\r';
    }
    Paste {
        target,
        bytes: normalized.into_iter(),
    }
}

pub fn paste() {
    let options = crate::utility::clipboard_options() | u64::from(crate::foreground::editing());
    paste_with_options(options);
}

pub fn paste_with_options(options: u64) {
    let clipboard = unsafe { &mut *SLOT.0.get() };
    let target = Target {
        pid: crate::scheduler::pid(),
        editor: crate::foreground::editing(),
        multiline: options & 1 != 0,
        block: options & 4 != 0,
    };
    clipboard.ready = None;
    clipboard.request = None;
    if let Some(host) = &mut clipboard.host {
        clipboard.sequence = clipboard.sequence.wrapping_add(1);
        let id = clipboard.sequence;
        if host
            .send_bytes(alloc::format!("P\t{id}\n").as_bytes())
            .is_ok()
        {
            clipboard.request = Some(Request {
                id,
                target,
                deadline: monitor::ticks() + monitor::seconds_to_ticks(3),
            });
            return;
        }
    }
    clipboard.ready = Some(prepare(&clipboard.local, target));
}

pub fn cancel_paste() {
    let clipboard = unsafe { &mut *SLOT.0.get() };
    let pid = crate::scheduler::pid();
    if clipboard.request.as_ref().is_some_and(|r| r.target.pid == pid) {
        clipboard.request = None;
    }
    if clipboard.ready.as_ref().is_some_and(|p| p.target.pid == pid) {
        clipboard.ready = None;
    }
}

fn decode(field: &[u8]) -> Option<Vec<u8>> {
    let pairs = field.chunks_exact(2);
    if !pairs.remainder().is_empty() || field.len() > 2 * LIMIT {
        return None;
    }
    let digit = |byte| char::from(byte).to_digit(16).map(|n| n as u8);
    pairs
        .map(|pair| Some(digit(pair[0])? * 16 + digit(pair[1])?))
        .collect()
}

fn poll(clipboard: &mut Clipboard) {
    if let Some(host) = &mut clipboard.host {
        for _ in 0..512 {
            let Ok(Some(byte)) = host.recv(true) else {
                break;
            };
            if byte != b'\n' {
                if clipboard.reply.len() == 2 * LIMIT + 21 {
                    clipboard.overflow = true;
                } else if !clipboard.overflow {
                    clipboard.reply.push(byte);
                }
                continue;
            }
            if !clipboard.overflow {
                if let Some(size) = clipboard.reply.strip_prefix(b"R\t") {
                    if let Some((width, height)) = core::str::from_utf8(size)
                        .ok()
                        .and_then(|text| text.split_once('\t'))
                        .and_then(|(width, height)| {
                            Some((width.parse::<usize>().ok()?, height.parse::<usize>().ok()?))
                        })
                    {
                        crate::graphics::request_resize(width, height);
                    }
                }
                let response = core::str::from_utf8(&clipboard.reply)
                    .ok()
                    .and_then(|line| line.split_once('\t'))
                    .and_then(|(id, data)| {
                        Some((id.parse::<u64>().ok()?, decode(data.as_bytes())?))
                    });
                if let Some((id, bytes)) = response {
                    if clipboard
                        .request
                        .as_ref()
                        .is_some_and(|request| request.id == id)
                    {
                        let request = clipboard.request.take().unwrap();
                        clipboard.ready = Some(prepare(&bytes, request.target));
                    }
                }
            }
            clipboard.reply.clear();
            clipboard.overflow = false;
        }
    }
    if clipboard
        .request
        .as_ref()
        .is_some_and(|request| monitor::ticks() >= request.deadline)
    {
        let request = clipboard.request.take().unwrap();
        clipboard.ready = Some(prepare(&clipboard.local, request.target));
    }
}

pub fn take_text() -> Option<Vec<u8>> {
    let clipboard = unsafe { &mut *SLOT.0.get() };
    poll(clipboard);
    let paste = clipboard.ready.as_ref()?;
    if paste.target.pid != crate::scheduler::pid() || !paste.target.block {
        return None;
    }
    let mut bytes: Vec<u8> = clipboard.ready.take()?.bytes.collect();
    if let Err(error) = core::str::from_utf8(&bytes) {
        if error.error_len().is_some() {
            return None;
        }
        bytes.truncate(error.valid_up_to());
    }
    Some(bytes)
}

pub fn input() -> Option<u8> {
    let clipboard = unsafe { &mut *SLOT.0.get() };
    poll(clipboard);
    let paste = clipboard.ready.as_mut()?;
    if paste.target.pid != crate::scheduler::pid() {
        return None;
    }
    if paste.target.block {
        return None;
    }
    if paste.target.editor != crate::foreground::editing() {
        clipboard.ready = None;
        return None;
    }
    let byte = paste.bytes.next();
    if byte.is_none() {
        clipboard.ready = None;
    }
    byte
}
