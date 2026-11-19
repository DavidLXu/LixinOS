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

struct Paste {
    pid: usize,
    multiline: bool,
    bytes: alloc::vec::IntoIter<u8>,
}

struct Request {
    id: u64,
    pid: usize,
    multiline: bool,
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

fn prepare(bytes: &[u8], pid: usize, multiline: bool) -> Paste {
    let mut normalized = Vec::with_capacity(bytes.len());
    let mut previous_cr = false;
    for &byte in bytes {
        match byte {
            b'\n' if previous_cr => (),
            b'\r' | b'\n' => normalized.push(if multiline { b'\r' } else { b' ' }),
            b'\t' => normalized.push(if multiline { b'\t' } else { b' ' }),
            32..=126 => normalized.push(byte),
            _ => (),
        }
        previous_cr = byte == b'\r';
    }
    Paste {
        pid,
        multiline,
        bytes: normalized.into_iter(),
    }
}

pub fn paste() {
    let clipboard = unsafe { &mut *SLOT.0.get() };
    let pid = crate::scheduler::pid();
    let multiline = crate::foreground::editing();
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
                pid,
                multiline,
                deadline: monitor::ticks() + monitor::seconds_to_ticks(3),
            });
            return;
        }
    }
    clipboard.ready = Some(prepare(&clipboard.local, pid, multiline));
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

pub fn input() -> Option<u8> {
    let clipboard = unsafe { &mut *SLOT.0.get() };
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
                        clipboard.ready = Some(prepare(&bytes, request.pid, request.multiline));
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
        clipboard.ready = Some(prepare(&clipboard.local, request.pid, request.multiline));
    }
    let paste = clipboard.ready.as_mut()?;
    if paste.pid != crate::scheduler::pid() {
        return None;
    }
    if paste.multiline != crate::foreground::editing() {
        clipboard.ready = None;
        return None;
    }
    let byte = paste.bytes.next();
    if byte.is_none() {
        clipboard.ready = None;
    }
    byte
}
