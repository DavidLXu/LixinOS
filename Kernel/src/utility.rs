// Small stand-alone AArch64 utilities use an ELF64 image with one fixed RX segment.
use crate::{disk, process::{Entry, Volume}, user};
use alloc::{rc::Rc, string::String, vec::Vec};
use core::{
    cell::{RefCell, UnsafeCell},
    slice,
};

pub fn installed(name: &str) -> Option<&'static [u8]> {
    Some(match name {
        "links" => include_bytes!("../target/user-build/links.elf"),
        "lust" => include_bytes!("../target/user-build/lust.elf"),
        "ls" => include_bytes!("../target/user-build/ls.elf"),
        "echo" => include_bytes!("../target/user-build/core-echo.elf"),
        "cat" => include_bytes!("../target/user-build/core-cat.elf"),
        "grep" => include_bytes!("../target/user-build/core-grep.elf"),
        "wc" => include_bytes!("../target/user-build/core-wc.elf"),
        "ssh" => include_bytes!("../target/user-build/ssh.elf"),
        "desktop" => include_bytes!("../target/user-build/desktop.elf"),
        "netinfo" => include_bytes!("../target/user-build/core-netinfo.elf"),
        "cpuinfo" => include_bytes!("../target/user-build/core-cpuinfo.elf"),
        "sysinfo" => include_bytes!("../target/user-build/core-sysinfo.elf"),
        "uname" => include_bytes!("../target/user-build/core-uname.elf"),
        "pwd" => include_bytes!("../target/user-build/core-pwd.elf"),
        "head" => include_bytes!("../target/user-build/core-head.elf"),
        "tail" => include_bytes!("../target/user-build/core-tail.elf"),
        "tee" => include_bytes!("../target/user-build/core-tee.elf"),
        "true" => include_bytes!("../target/user-build/core-true.elf"),
        "false" => include_bytes!("../target/user-build/core-false.elf"),
        "sleep" => include_bytes!("../target/user-build/core-sleep.elf"),
        "sort" => include_bytes!("../target/user-build/core-sort.elf"),
        "uniq" => include_bytes!("../target/user-build/core-uniq.elf"),
        "date" => include_bytes!("../target/user-build/core-date.elf"),
        "find" => include_bytes!("../target/user-build/core-find.elf"),
        "ps" => include_bytes!("../target/user-build/core-ps.elf"),
        "df" => include_bytes!("../target/user-build/core-df.elf"),
        "stat" => include_bytes!("../target/user-build/core-stat.elf"),
        "tree" => include_bytes!("../target/user-build/core-tree.elf"),
        "du" => include_bytes!("../target/user-build/core-du.elf"),
        "cmp" => include_bytes!("../target/user-build/core-cmp.elf"),
        "diff" => include_bytes!("../target/user-build/core-diff.elf"),
        "nl" => include_bytes!("../target/user-build/core-nl.elf"),
        "cut" => include_bytes!("../target/user-build/core-cut.elf"),
        "tr" => include_bytes!("../target/user-build/core-tr.elf"),
        "sed" => include_bytes!("../target/user-build/core-sed.elf"),
        "seq" => include_bytes!("../target/user-build/core-seq.elf"),
        "basename" => include_bytes!("../target/user-build/core-basename.elf"),
        "dirname" => include_bytes!("../target/user-build/core-dirname.elf"),
        "test" => include_bytes!("../target/user-build/core-test.elf"),
        "printf" => include_bytes!("../target/user-build/core-printf.elf"),
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

pub(crate) fn load_segments(file: &[u8]) -> Option<(&[u8], &[u8])> {
    if let Some(large) = user::large_program(file) { return Some((large.entry, &[])); }
    segment(file).map(|code| (code, &[][..])).or_else(|| user::application_segments(file))
}

struct DirectoryCursor {
    directory: String,
    name: String,
    next_index: u64,
    generation: u64,
}

#[derive(Clone, Default)]
pub(super) struct Streams {
    buffer: Rc<RefCell<StreamData>>,
    closed:[bool;3],
    null:[bool;3],
    environment: Option<Rc<[u8]>>,
    pub input_pipe: Option<Rc<crate::pipe::Reader>>,
    pub error_pipe: Option<Rc<crate::pipe::Writer>>,
    pub output_pipe: Option<Rc<crate::pipe::Writer>>,
}
#[derive(Default)]
struct StreamData {
    output: Option<Vec<u8>>,
    error: Option<&'static str>,
    input: Option<alloc::vec::IntoIter<u8>>,
}
impl StreamData {
    fn capture(&mut self, bytes: &[u8]) -> Option<bool> {
        let out = self.output.as_mut()?;
        if self.error.is_some() { return Some(false); }
        let end = out.len() + bytes.len();
        if end > disk::FILE_LIMIT {
            self.error = Some("file too large");
            return Some(false);
        }
        if end > out.capacity() {
            let capacity = end.max((2 * out.capacity()).min(disk::FILE_LIMIT));
            if out.try_reserve_exact(capacity - out.len()).is_err() {
                self.error = Some("not enough file buffer memory");
                return Some(false);
            }
        }
        out.extend_from_slice(bytes);
        Some(true)
    }
}
impl Streams {
    pub fn new(input: Option<String>, capture: bool) -> Self {
        Self::from_bytes(input.map(String::into_bytes), capture)
    }
    pub fn from_bytes(input: Option<Vec<u8>>, capture: bool) -> Self {
        Self {
            buffer: Rc::new(RefCell::new(StreamData {
                output: capture.then(Vec::new),
                input: input.map(Vec::into_iter),
                error: None,
            })),
            closed:[false;3],
            null:[false;3],
            environment: None,
            input_pipe: None,
            output_pipe: None, error_pipe: None,
        }
    }
    pub fn capture_view(&self)->Self {Self{buffer:self.buffer.clone(),..Self::default()}}
    pub fn output_bytes(&self) -> Result<Vec<u8>, &'static str> {
        let mut data = self.buffer.borrow_mut();
        if let Some(error) = data.error { return Err(error); }
        Ok(data.output.take().unwrap_or_default())
    }
    pub fn output(&self) -> Result<String, &'static str> {
        String::from_utf8(self.output_bytes()?).map_err(|_| "invalid command output")
    }
    pub fn redirected(&self) -> bool {
        let data = self.buffer.borrow();
        self.input_pipe.is_some()
            || self.output_pipe.is_some()
            || data.input.is_some()
            || data.output.is_some()
    }
}

enum Handle {
    File { data: Vec<u8>, position: usize },
    Writable {
        path: String,
        position: usize,
        append: bool,
    },
    Socket(u64),
    PipeRead(Rc<crate::pipe::Reader>),
    PipeWrite(Rc<crate::pipe::Writer>),
}

pub(super) struct Context {
    heap: user::UserArena,
    extended: user::UserArena,
    program: user::UserArena,
    wide: user::UserArena,
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
    pub(super) executable: String,
    peeked:Option<u8>,
    pub streams: Streams,
    // Match the 32 guest descriptors in the musl adapter, in addition to stdio.
    handles: [Option<Handle>; 32],
    fs_batch: Option<crate::fs_batch::Batch>,
}
struct Slot(UnsafeCell<Option<Context>>);
unsafe impl Sync for Slot {}
static SLOTS: [Slot; user::BANKS] = [const { Slot(UnsafeCell::new(None)) }; user::BANKS];

pub fn compiler_file(descriptor: u64) -> Option<Vec<u8>> {
    let io = unsafe { (&*SLOTS[user::bank()].0.get()).as_ref() };
    user::copy_user_file(descriptor, io.map(|ctx| &ctx.heap))
}

pub fn active() -> bool { unsafe { (&*SLOTS[user::bank()].0.get()).is_some() } }

pub(super) fn command_context() -> Option<(String, Rc<RefCell<Volume>>, Streams)> {
    let context = unsafe { (&*SLOTS[user::bank()].0.get()).as_ref()? };
    Some((context.cwd.clone(), context.volume.clone(), context.streams.clone()))
}

#[cfg(feature = "retina")]
pub(super) fn owns_canvas() -> bool {
    unsafe { (&*SLOTS[user::bank()].0.get()).as_ref().is_some_and(|io| io.canvas.is_some()) }
}

impl Context {
    pub fn new(cwd: &str, volume: Rc<RefCell<Volume>>, streams: Streams) -> Self {
        Self {
            #[cfg(feature = "retina")]
            allow_canvas: false,
            #[cfg(feature = "retina")]
            canvas: None,
            #[cfg(feature="retina")]
            popup_shell: false,
            heap: user::UserArena::for_browser_heap(),
            extended: user::UserArena::extended(),
            program: user::UserArena::for_program(),
            wide: user::UserArena::wide(),
            executable: String::new(),
            connections: Default::default(), peeked:None,cursor: None, volume, cwd: cwd.into(), streams, handles: core::array::from_fn(|_| None), fs_batch: None }
    }
    // Move open handles and the directory cursor back to the caller on yield.
    // Each suspended process therefore retains its own descriptors.
    pub fn enter<T>(self, launch: impl FnOnce() -> T) -> (Self, T) {
        let slot = SLOTS[user::bank()].0.get();
        self.heap.set_mapped(true);
        self.extended.set_mapped(true);
        self.program.set_mapped(true);
        self.wide.set_mapped(true);
        unsafe { *slot = Some(self); }
        let result = launch();
        let io = unsafe { (*slot).take().unwrap() };
        io.heap.set_mapped(false);
        io.extended.set_mapped(false);
        io.program.set_mapped(false);
        io.wide.set_mapped(false);
        (io, result)
    }
}

pub(super) fn launch_program(file: &[u8], cwd: &str, args: &str) -> Result<u16, &'static str> {
    let (text, data) = load_segments(file).ok_or("invalid LixinOS executable")?;
    if let Some(large) = user::large_program(file) {
        let io = unsafe { (&mut *SLOTS[user::bank()].0.get()).as_mut() }.ok_or("missing program context")?;
        large.load(&mut io.program)?;
    }
    user::launch_segments(text, data, Some((cwd, args)), false)
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

fn standard_input(ctx: &mut Context, raw: bool) -> Option<u8> {
    if let Some(reader)=&ctx.streams.input_pipe {return reader.read();}
    // Release the shared stream borrow before another process can run.
    let buffered=ctx.streams.buffer.borrow_mut().input.as_mut().map(Iterator::next);
    if let Some(next)=buffered{return next;}
    let b=ctx.peeked.take().unwrap_or_else(crate::read_byte);
    match (raw,b) {(false,4)=>None,(false,13)=>Some(10),_=>Some(b)}
}

pub fn io(value: u64, operation: u64) -> Option<u64> {
    let context = unsafe { (&mut *SLOTS[user::bank()].0.get()).as_mut()? };
    match operation {
        49 => {
            let mut free = context.handles.iter().enumerate().filter(|(_, h)| h.is_none()).map(|(i, _)| i);
            let (Some(r), Some(w)) = (free.next(), free.next()) else { return Some(u64::MAX); };
            let (reader, writer) = crate::pipe::pair();
            context.handles[r] = Some(Handle::PipeRead(reader));
            context.handles[w] = Some(Handle::PipeWrite(writer));
            Some(((r+3) as u64) << 32 | (w+3) as u64)
        }
        50 => {
            let len = (value >> 32) as usize;
            if !(4..=32768).contains(&len) { return Some(u64::MAX); }
            let raw = unsafe { slice::from_raw_parts(user::git_mailbox(), len) };
            let mut streams = context.streams.clone();
            let raw = if value & (1 << 24) != 0 {
                let n = u16::from_le_bytes([raw[0], raw[1]]) as usize;
                let Some(env) = raw.get(2..2+n) else { return Some(u64::MAX) };
                if n > 8192 || (n > 0 && env.last() != Some(&0)) { return Some(u64::MAX); }
                streams.environment = Some(Rc::from(env));
                &raw[2+n..]
            } else { raw };
            let Some(raw) = raw.strip_suffix(&[0]) else { return Some(u64::MAX); };
            let Ok(text) = core::str::from_utf8(raw) else { return Some(u64::MAX); };
            let mut words = text.split('\0');
            let Some(cwd) = words.next() else {return Some(u64::MAX)};
            let Some(path) = words.next() else {return Some(u64::MAX)};
            let args: Vec<String> = words.map(String::from).collect();
            if args.is_empty() || args.len() > 512 { return Some(u64::MAX); }
            let volume = context.volume.borrow();
            let Ok(cwd) = volume.resolve(&context.cwd, cwd) else {return Some(u64::MAX)};
            if !matches!(volume.entry(&cwd), Some(Entry::Directory)) {return Some(u64::MAX)}
            let Ok(path) = volume.resolve(&cwd, path) else {return Some(u64::MAX)};
            drop(volume);
            for stdio in 0..3 {
                let fd = ((value >> (8*stdio)) & 255) as usize;
                if fd == stdio {continue}
                streams.null[stdio] = fd == 255;
                if fd == 255 {
                    streams.closed[stdio] = false;
                    match stdio {
                        0 => streams.input_pipe = None,
                        1 => streams.output_pipe = None,
                        _ => streams.error_pipe = None,
                    }
                    continue;
                }
                if fd == 1 && stdio == 2 { streams.error_pipe = streams.output_pipe.clone(); streams.null[2] = streams.null[1]; continue; }
                let h = fd.checked_sub(3).and_then(|i| context.handles.get(i)).and_then(Option::as_ref);
                match (stdio, h) {
                    (0, Some(Handle::PipeRead(r))) => streams.input_pipe = Some(r.clone()),
                    (1, Some(Handle::PipeWrite(w))) => streams.output_pipe = Some(w.clone()),
                    (2, Some(Handle::PipeWrite(w))) => streams.error_pipe = Some(w.clone()),
                    _ => return Some(u64::MAX),
                }
                streams.closed[stdio] = false;
            }
            Some(crate::process::spawn_program(cwd, context.volume.clone(), streams, path, args)
                .map_or(u64::MAX, |pid| pid as u64))
        }
        51 => Some(if value >> 32 == 0 {
            crate::scheduler::wait(value as usize).map_or(u64::MAX, u64::from)
        } else {
            crate::scheduler::try_wait(value as u32 as usize)
                .map_or((-2i64) as u64, |r| r.map_or(u64::MAX, u64::from))
        }),
        47 => {
            let fd = ((value >> 32) as u32 & 0x7fffffff) as usize;
            let handle = fd.checked_sub(3).and_then(|i| context.handles.get_mut(i)).and_then(Option::as_mut);
            let size = value as u32 as usize;
            if value >> 63 == 0 {
                return Some(match handle {
                    Some(Handle::File { position, .. } | Handle::Writable { position, .. }) => {
                        *position = size;
                        0
                    }
                    _ => u64::MAX,
                });
            }
            let Some(Handle::Writable { path, .. }) = handle else { return Some(u64::MAX); };
            let mut volume = context.volume.borrow_mut();
            if size > volume.disk.file_limit() { return Some(u64::MAX); }
            let Some(Some(mut data)) = volume.files.get(path).cloned() else { return Some(u64::MAX); };
            if data.try_reserve(size.saturating_sub(data.len())).is_err() { return Some(u64::MAX); }
            data.resize(size, 0);
            Some(if volume.commit(path.clone(), Some(Some(data)), None).is_ok() { 0 } else { u64::MAX })
        }
        48 => {
            let mail = unsafe { slice::from_raw_parts(user::utility_mailbox(), 254) };
            let name = mail.iter().position(|&b| b == 0).and_then(|n| core::str::from_utf8(&mail[..n]).ok())?;
            Some(crate::net::resolve(name).map(|ip| u32::from_be_bytes(ip.octets()) as u64).unwrap_or(u64::MAX))
        }
        46 => Some(if context.heap.seal_text(value as usize) { 0 } else { u64::MAX }),
        45 => Some(fs_batch_call(context, value)),
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
            if context.streams.closed[0] || context.streams.null[0] {return Some(256);}
            #[cfg(feature = "split")]
            crate::process::ready_on_read();
            #[cfg(feature="retina")]
            if context.popup_shell && value == 1 {
                return Some(crate::graphics::popup_key());
            }
            let byte=standard_input(context,value==1);
            Some(byte.map_or(256, u64::from))
        }
        1 => {
            if context.streams.closed[1]{return Some((-2i64) as u64);}
            if context.streams.null[1] {return Some(value & 255);}
            if let Some(writer)=&context.streams.output_pipe {return Some(if writer.write(value as u8) {value & 255} else {(-2i64) as u64});}
            if let Some(ok) = context.streams.buffer.borrow_mut().capture(&[value as u8]) {
                return Some(if ok { value & 255 } else { (-2i64) as u64 });
            }
            if value & 256 != 0 {
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
                    let path = volume.resolve(&context.cwd, name).ok()?;
                    volume.read_file(&path).ok()
                });
            let fd = context.handles.iter().position(Option::is_none);
            Some(match (data, fd) {
                (Some(bytes), Some(fd)) => {
                    context.handles[fd] = Some(Handle::File { data: bytes, position: 0 });
                    fd as u64 + 3
                }
                _ => u64::MAX,
            })
        }
        9 | 39 => {
            let fd = (value >> 32) as usize;
            let count = value as u32 as usize;
            if fd<3 && context.streams.closed[fd] {return Some(u64::MAX);}
            if fd < 3 && context.streams.null[fd] && count <= 4096 { return Some(if operation == 9 { 0 } else { count as u64 }); }
            if count > 4096 {
                return Some(u64::MAX);
            }
            let buffer = unsafe { slice::from_raw_parts_mut(user::utility_mailbox(), count) };
            if count == 0 {
                return Some(0);
            }
            if fd == 0 && operation == 9 {
                if let Some(reader) = &context.streams.input_pipe {
                    return Some(reader.read_into(buffer) as u64);
                }
                let byte=standard_input(context,false);
                return Some(byte.map_or(0, |b| {
                    buffer[0] = b;
                    1
                }));
            }
            if (fd == 1 || fd == 2) && operation == 39 {
                {
                    if let Some(w) = if fd == 1 { &context.streams.output_pipe } else { &context.streams.error_pipe } {
                        return Some(w.write_from(buffer).map_or(u64::MAX, |n| n as u64));
                    }
                }
                if fd == 1 {
                    if let Some(ok) = context.streams.buffer.borrow_mut().capture(buffer) {
                        return Some(if ok { count as u64 } else { u64::MAX });
                    }
                }
                for &byte in buffer.iter() {
                    terminal_byte(byte);
                }
                return Some(count as u64);
            }
            let Some(handle) = fd
                .checked_sub(3)
                .and_then(|i| context.handles.get_mut(i))
                .and_then(Option::as_mut)
            else {
                return Some(u64::MAX);
            };
            match handle {
                Handle::PipeRead(r) if operation == 9 => Some(r.read_into(buffer) as u64),
                Handle::PipeWrite(w) if operation == 39 => Some(w.write_from(buffer).map_or(u64::MAX, |n| n as u64)),
                Handle::File { data, position } if operation == 9 => {
                    let bytes = data.get(*position..).unwrap_or_default();
                    let n = count.min(bytes.len());
                    buffer[..n].copy_from_slice(&bytes[..n]);
                    *position += n;
                    Some(n as u64)
                }
                Handle::Writable { path, position, .. } if operation == 9 => {
                    let volume = context.volume.borrow();
                    let Some(Some(data)) = volume.files.get(path) else { return Some(u64::MAX); };
                    let bytes = data.get(*position..).unwrap_or_default();
                    let n = count.min(bytes.len());
                    buffer[..n].copy_from_slice(&bytes[..n]);
                    *position += n;
                    Some(n as u64)
                }
                Handle::Writable {
                    path,
                    position,
                    append,
                } if operation == 39 => {
                    let mut volume = context.volume.borrow_mut();
                    if volume.disk.scratch_path(path) && !volume.versions.contains_key(path) {
                        let Volume { files, disk, .. } = &mut *volume;
                        let Some(Some(file)) = files.get_mut(path) else { return Some(u64::MAX); };
                        let start = if *append { file.len() } else { *position };
                        if count != 0 && disk.write_scratch(path, file, start, buffer).is_err() {
                            return Some(u64::MAX);
                        }
                        *position = start + count;
                        return Some(count as u64);
                    }
                    let Some(Some(mut bytes)) = volume.files.get(path).cloned() else {
                        return Some(u64::MAX);
                    };
                    let start = if *append { bytes.len() } else { *position };
                    let end = start + count;
                    if end > volume.disk.file_limit() {
                        return Some(u64::MAX);
                    }
                    bytes.resize(bytes.len().max(end), 0);
                    bytes[start..end].copy_from_slice(buffer);
                    if volume
                        .commit(path.clone(), Some(Some(bytes)), None)
                        .is_err()
                    {
                        return Some(u64::MAX);
                    }
                    *position = end;
                    Some(count as u64)
                }
                Handle::Socket(socket) => loop {
                    let result = context.connections.call(
                        if operation == 9 { 26 } else { 27 },
                        *socket << 32 | count as u64,
                        buffer,
                    );
                    if result != -2 {
                        break Some(result as u64);
                    }
                    crate::scheduler::sleep_ms(2);
                },
                _ => Some(u64::MAX),
            }
        }
        10 | 28 => {
            let fd = if operation == 10 { value } else { value >> 32 };
            if fd<3 && operation==10 {
                if core::mem::replace(&mut context.streams.closed[fd as usize],true){return Some(u64::MAX);}
                match fd {0=>{context.streams.input_pipe=None;},1=>{context.streams.output_pipe=None;},2=>{context.streams.error_pipe=None;},_=>()}
                return Some(0);
            }
            let closed = fd
                .checked_sub(3)
                .and_then(|i| context.handles.get_mut(i as usize))
                .and_then(Option::take);
            Some(match closed {
                Some(Handle::Socket(s)) => context.connections.call(28, s << 32, &mut []) as u64,
                Some(_) => 0,
                None => u64::MAX,
            })
        }
        40 => {
            let Some(fd) = context.handles.iter().position(Option::is_none) else {
                return Some(u64::MAX);
            };
            let mail = unsafe { slice::from_raw_parts(user::utility_mailbox(), 513) };
            let name = mail
                .iter()
                .position(|&b| b == 0)
                .and_then(|n| core::str::from_utf8(&mail[..n]).ok());
            let mut volume = context.volume.borrow_mut();
            let Some(path) =
                name.and_then(|name| volume.resolve(&context.cwd, name).ok())
            else {
                return Some(u64::MAX);
            };
            if volume.require_writable(&path).is_err() || volume.files.get(&path) == Some(&None) {
                return Some(u64::MAX);
            }
            let append = value == 1;
            if (value == 0 || !volume.files.contains_key(&path))
                && volume
                    .commit(path.clone(), Some(Some(Vec::new())), None)
                    .is_err()
            {
                return Some(u64::MAX);
            }
            context.handles[fd] = Some(Handle::Writable {
                path,
                position: 0,
                append,
            });
            Some(fd as u64 + 3)
        }
        41 => {
            let fd = (value >> 48) as usize;
            let mask = (value >> 32) as u8;
            let timeout = value as u32;
            let deadline = crate::monitor::ticks()
                + crate::monitor::seconds_to_ticks(1) / 1000 * u64::from(timeout);
            if (fd<3 && context.streams.closed[fd]) || mask == 0 || mask & !3 != 0 {
                return Some(u64::MAX);
            }
            loop {
                let ready = match fd {
                    0..=2 if context.streams.null[fd] => if fd == 0 { 1 } else { 2 },
                    0 if context.streams.input_pipe.is_some() => {
                        u8::from(context.streams.input_pipe.as_ref().unwrap().ready())
                    }
                    0 if context.streams.buffer.borrow().input.is_some()
                        || crate::scheduler::background() || crate::process::agent_noninteractive() =>
                    {
                        1
                    }
                    0 => {
                        if context.peeked.is_none() {
                            context.peeked = crate::poll_byte();
                        }
                        u8::from(context.peeked.is_some())
                    }
                    1 => {
                        if context
                            .streams
                            .output_pipe
                            .as_ref()
                            .is_none_or(|w| w.ready())
                        {
                            2
                        } else {
                            0
                        }
                    }
                    2 => 2,
                    fd => match context.handles.get(fd - 3).and_then(Option::as_ref) {
                        Some(Handle::PipeRead(r)) => u8::from(r.ready()),
                        Some(Handle::PipeWrite(w)) => if w.ready() { 2 } else { 0 },
                        Some(Handle::File { .. }) => 1,
                        Some(Handle::Writable { .. }) => 2,
                        Some(Handle::Socket(s)) => {
                            context.connections.ready(*s as usize).unwrap_or(3)
                        }
                        None => return Some(u64::MAX),
                    },
                } & mask;
                if ready != 0 || (timeout != u32::MAX && crate::monitor::ticks() >= deadline) {
                    return Some(ready.into());
                }
                crate::scheduler::sleep_ms(1);
            }
        }

        11 => {
            if context.streams.closed[2]{return Some(u64::MAX);}
            if context.streams.null[2] { return Some(value & 255); }
            if let Some(w) = &context.streams.error_pipe { return Some(if w.write(value as u8) {value & 255} else {u64::MAX}); }
            terminal_byte(value as u8);
            Some(value & 255)
        }
        25..=27 => {
            let mail = unsafe { slice::from_raw_parts_mut(user::utility_mailbox(), 4096) };
            if operation==25 {
                let Some(fd)=context.handles.iter().position(Option::is_none) else{return Some(u64::MAX);};
                let socket=context.connections.call(25,value,mail);
                if socket<0 {return Some(u64::MAX);}
                context.handles[fd]=Some(Handle::Socket(socket as u64));
                return Some(fd as u64+3);
            }
            let Some(Handle::Socket(socket))=(value>>32).checked_sub(3)
                .and_then(|fd|context.handles.get(fd as usize)).and_then(Option::as_ref) else{return Some(u64::MAX);};
            Some(context.connections.call(operation,*socket<<32|value as u32 as u64,mail) as u64)
        }
        29 => {
            use crate::inspect;
            let text = match value {
                0 => {
                    let Some(text) = crate::net::info() else { return Some(u64::MAX) };
                    text
                }
                1 => inspect::SYSTEM_NAME.into(),
                2 => inspect::SYSTEM_RELEASE.into(),
                3 => inspect::ARCHITECTURE.into(),
                4 => inspect::system_summary(),
                5 => {
                    let volume = context.volume.borrow();
                    alloc::format!("{}\n{}\n{}\n{}", inspect::system(), inspect::memory(),
                        inspect::disk(&volume.disk), inspect::filesystem(&volume))
                }
                6 => crate::scheduler::process_list(),
                7 => {
                    let volume = context.volume.borrow();
                    let used = volume.disk.used_bytes();
                    let capacity = volume.disk.capacity_bytes();
                    alloc::format!("Filesystem Capacity Used Available\n/ {capacity} {used} {}\n",
                        capacity.saturating_sub(used))
                }
                8 => inspect::cpu(),
                _ => return Some(u64::MAX),
            };
            let mail = unsafe { slice::from_raw_parts_mut(user::utility_mailbox(), 4096) };
            if text.len() > mail.len() { return Some(u64::MAX); }
            mail[..text.len()].copy_from_slice(text.as_bytes());
            Some(text.len() as u64)
        }
        // Monotonic time from the ARM virtual counter. Mode 0 is the original
        // millisecond API, mode 1 is microseconds, and mode 2 is raw ticks.
        38 => {
            let ticks = crate::monitor::ticks();
            let frequency = crate::monitor::seconds_to_ticks(1);
            Some(match value {
                0 => ticks / frequency * 1000 + ticks % frequency * 1000 / frequency,
                1 => ticks / frequency * 1_000_000 + ticks % frequency * 1_000_000 / frequency,
                2 => ticks,
                _ => u64::MAX,
            })
        }
        54 => Some(crate::scheduler::pid() as u64),
        53 => Some(match context.streams.environment.as_ref() {
            None => 0,
            Some(env) => {
                unsafe { core::ptr::copy_nonoverlapping(env.as_ptr(), user::git_mailbox(), env.len()); }
                env.len() as u64 + 1
            }
        }),
        52 => Some(if context.extended.grow(value as usize).is_ok() { user::EXTENDED_BASE as u64 } else { u64::MAX }),
        37 if value == u64::MAX => Some(user::UserArena::browser_heap_limit() as u64),
        37 if value >> 40 == 1 => Some(if crate::high_heap_end().is_some() && context.wide.grow((value - (1 << 40)) as usize).is_ok() {
            user::WIDE_BASE as u64
        } else { u64::MAX }),
        37 => Some(if context.heap.grow(value as usize).is_ok() { user::WEB_HEAP_BASE as u64 } else { u64::MAX }),
        // QEMU's PL031 RTC counts seconds since the Unix epoch.
        36 => Some(wall_seconds() as u64),
        30 => {
            if value > 4096 { return Some(u64::MAX); }
            let mail = unsafe { slice::from_raw_parts_mut(user::utility_mailbox(), value as usize) };
            Some(if crate::entropy::fill(mail).is_some() { 0 } else { u64::MAX })
        }
        // Guest directory operations share the active process's working directory.
        42 => {
            let mail = unsafe { slice::from_raw_parts_mut(user::utility_mailbox(), 513) };
            if value == 6 { return Some((3 + context.handles.len()) as u64); }
            if value == 5 {
                mail[..context.executable.len()].copy_from_slice(context.executable.as_bytes());
                return Some(context.executable.len() as u64);
            }
            if value == 0 {
                mail[..context.cwd.len()].copy_from_slice(context.cwd.as_bytes());
                return Some(context.cwd.len() as u64);
            }
            let name = mail.iter().position(|&b| b == 0)
                .and_then(|n| core::str::from_utf8(&mail[..n]).ok());
            let mut volume = context.volume.borrow_mut();
            let Some(path) = name.and_then(|name| volume.resolve(&context.cwd, name).ok())
            else { return Some((-22i64) as u64); };
            if matches!(value, 2..=4) && volume.require_writable(&path).is_err() {
                return Some((-30i64) as u64);
            }
            if value == 1 && matches!(volume.entry(&path), Some(Entry::Directory)) {
                context.cwd = path;
                context.cursor = None;
                return Some(0);
            }
            let current = volume.files.get(&path);
            let result = match value {
                1 => if volume.entry(&path).is_some() { -20 } else { -2 },
                2 if current.is_some() => -17,
                2 => if volume.commit(path, Some(None), None).is_ok() { 0 } else { -5 },
                3 if current == Some(&None) => -21,
                3 if current.is_none() => -2,
                3 => if volume.commit(path, None, None).is_ok() { 0 } else { -5 },
                4 if current != Some(&None) => if current.is_some() { -20 } else { -2 },
                4 if path == "/" || context.cwd == path
                    || context.cwd.strip_prefix(&path).is_some_and(|suffix| suffix.starts_with('/')) => -16,
                4 if disk::children(&volume.files, &path).next().is_some() => -39,
                4 => if volume.commit(path, None, None).is_ok() { 0 } else { -5 },
                _ => -22,
            };
            Some(result as u64)
        }
        43 => {
            if value > 3_600_000 { return Some(u64::MAX); }
            crate::scheduler::sleep_ms(value);
            Some(0)
        }
        24 if value == 2 => Some(u64::from(!context.streams.null[0] && !context.streams.closed[0] && context.streams.input_pipe.is_none() && context.streams.buffer.borrow().input.is_none())),
        24 if value == 1 => Some(u64::from(!context.streams.null[1] && !context.streams.redirected())),
        24 => Some(u64::from(context.streams.output_pipe.is_none() && context.streams.buffer.borrow().output.is_none())),
        // SVC 22 atomically stores a file from a sparse-buffer descriptor or
        // legacy mailbox length. SVC 23 returns the resolved path.
        22 | 23 | 31 => {
            let mail = unsafe { slice::from_raw_parts_mut(user::utility_mailbox(), 513) };
            let path = mail.iter().position(|&b| b == 0)
                .and_then(|n| core::str::from_utf8(&mail[..n]).ok())
                .and_then(|s| context.volume.borrow().resolve(&context.cwd, s).ok());
            let Some(path) = path else { return Some(u64::MAX) };
            if operation == 23 {
                mail[..path.len()].copy_from_slice(path.as_bytes());
                mail[path.len()] = 0;
                return Some(0);
            }

            let mut volume = context.volume.borrow_mut();
            if volume.require_writable(&path).is_err() { return Some(u64::MAX); }
            // Exclusive creation for the first accepted SSH host fingerprint.
            if operation == 31 && volume.files.contains_key(&path) { return Some(u64::MAX); }
            if volume.files.get(&path) == Some(&None) { return Some(u64::MAX); }
            let bytes = if operation == 22 && value == u64::MAX {
                // The wide descriptor is a contiguous private heap buffer.
                let range = unsafe { slice::from_raw_parts(user::utility_mailbox().add(520).cast::<usize>(), 2) };
                let (address, size) = (range[0], range[1]);
                if size > volume.disk.file_limit() || ![&context.heap, &context.extended, &context.wide]
                    .iter().any(|arena| arena.contains(address, size)) { return Some(u64::MAX); }
                let mut bytes = Vec::new();
                if bytes.try_reserve_exact(size).is_err() { return Some(u64::MAX); }
                bytes.extend_from_slice(unsafe { slice::from_raw_parts(address as *const u8, size) });
                bytes
            } else if value >> 32 != 0 {
                if value as u32 as usize > volume.disk.file_limit() { return Some(u64::MAX); }
                let Some(bytes) = user::copy_user_file(value, Some(&context.heap)) else { return Some(u64::MAX) };
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

fn fs_batch_call(context: &mut Context, value: u64) -> u64 {
    // Two NUL-terminated paths in a private mailbox. Failures replace the
    // request with a NUL-terminated error and discard the whole transaction.
    let mail = unsafe { slice::from_raw_parts_mut(user::utility_mailbox(), 1034) };
    let op = value & 255;
    let flags = value >> 8;
    let result = (|| -> Result<u64, &'static str> {
        match op {
            0 => { context.fs_batch = Some(crate::fs_batch::Batch::new(&context.volume.borrow())); }
            6 => context.fs_batch.take().ok_or("no filesystem transaction")?
                .commit(&mut context.volume.borrow_mut())?,
            7 => { context.fs_batch = None; }
            8 => return context.fs_batch.as_ref().ok_or("no filesystem transaction")?
                .exists(&context.volume.borrow(), fs_batch_path(&mail[..513])?).map(u64::from),
            1..=5 => {
                let (first, second) = mail.split_at(513);
                let path = fs_batch_path(first)?;
                let other = if op >= 4 { fs_batch_path(second)? } else { "" };
                context.fs_batch.as_mut().ok_or("no filesystem transaction")?
                    .edit(&context.volume.borrow(), &context.cwd, op, flags, path, other)?;
            }
            _ => return Err("invalid filesystem transaction request"),
        }
        Ok(0)
    })();
    match result {
        Ok(value) => value,
        Err(error) => {
            context.fs_batch = None;
            mail[..error.len()].copy_from_slice(error.as_bytes());
            mail[error.len()] = 0;
            u64::MAX
        }
    }
}

fn fs_batch_path(bytes: &[u8]) -> Result<&str, &'static str> {
    let end = bytes.iter().position(|&byte| byte == 0).ok_or("path too long")?;
    let path = core::str::from_utf8(&bytes[..end]).map_err(|_| "invalid path")?;
    if path == "/" { return Ok(path); }
    let children = path.strip_prefix('/').ok_or("expected absolute path")?;
    if children.split('/').any(|part| matches!(part, "" | "." | "..")) {
        return Err("expected canonical path");
    }
    Ok(path)
}

pub(crate) fn wall_seconds() -> u32 {
    unsafe { core::ptr::read_volatile(0x0901_0000 as *const u32) }
}

fn write_times(mail: &mut [u8], time: crate::disk::FileTime) {
    mail[..4].copy_from_slice(&time.birth.to_le_bytes());
    mail[4..].copy_from_slice(&time.modified.to_le_bytes());
}

fn metadata(context: &mut Context, index: u64) -> i64 {
    // Both fields are bounded, within a dedicated page of the active EL0 bank.
    let mailbox = unsafe { slice::from_raw_parts_mut(user::utility_mailbox(), 1034) };
    let (request, response) = mailbox.split_at_mut(513);
    let (response, times) = response.split_at_mut(513);
    times.fill(0);
    let Some(end) = request.iter().position(|&b| b == 0) else {
        return -5;
    };
    let Ok(name) = core::str::from_utf8(&request[..end]) else {
        return -5;
    };
    let volume = context.volume.borrow();
    let path = match volume.resolve(&context.cwd, name) {
        Ok(path) => path,
        Err("not a directory") => return -4,
        Err(_) => return -5,
    };
    let Some(entry) = volume.entry(&path) else {
        return -3;
    };
    if index == 0 {
        write_times(times, volume.disk.times.get(&path).copied().unwrap_or_default());
        return entry.size().map_or(-2, |size| size as i64);
    }
    if !matches!(entry, Entry::Directory) {
        return -4;
    }
    let generation = volume.disk.revision();
    let after = context.cursor.as_ref().filter(|c|
        c.directory == path && c.next_index == index && c.generation == generation
    ).map(|c| c.name.as_str());
    let skip = if after.is_some() { 0 } else { (index - 1) as usize };
    let Some((name, entry)) = volume.children(&path, "", after).nth(skip) else {
        return -1;
    };
    // Look up the child without changing the directory cursor or issuing another syscall.
    let child = if path == "/" { alloc::format!("/{name}") } else { alloc::format!("{path}/{name}") };
    write_times(times, volume.disk.times.get(&child).copied().unwrap_or_default());
    context.cursor = Some(DirectoryCursor {
        name: name.into(),
        directory: path, next_index: index + 1, generation,
    });
    if name.len() >= response.len() {
        return -5;
    }
    response[..name.len()].copy_from_slice(name.as_bytes());
    response[name.len()] = 0;
    entry.size().map_or(-2, |size| size as i64)
}
