//! Synchronous foreground processes. A waiting parent owns a private memory
//! snapshot; sibling panes and child applications never share its syscall slot.
#[cfg(feature="retina")]
#[path="desktop.rs"]
mod desktop;
#[path = "../../User/sh/abi.rs"]
mod abi;
use super::*;
use crate::{user, utility};
use core::cell::UnsafeCell;

struct Spawn {
    path: String,
    args: Vec<String>,
    flags: u32,
}
struct Context {
    services: Services,
    history: VecDeque<String>,
    pending: Option<Spawn>,
    stream: String,
    last: Result<u8>,
    error: &'static str,
    handshake: bool,
    io: Option<utility::Context>,
}
struct Slot(UnsafeCell<Option<Context>>);
unsafe impl Sync for Slot {}
static SLOTS: [Slot; user::BANKS] = [const { Slot(UnsafeCell::new(None)) }; user::BANKS];
pub(crate) fn active() -> bool {
    unsafe { (&*SLOTS[user::bank()].0.get()).is_some() }
}
#[cfg(feature = "split")]
pub(crate) fn ready_on_read() {
    let slot = SLOTS[user::bank()].0.get();
    if unsafe { (&mut *slot).as_mut().is_some_and(|ctx| core::mem::take(&mut ctx.handshake)) } {
        crate::panes::switch();
    }
}
impl Context {
    fn new(services: Services, history: VecDeque<String>, handshake: bool) -> Self {
        Self {
            services,
            history,
            handshake,
            io: None,
            pending: None,
            stream: String::new(),
            last: Ok(0),
            error: "invalid process request",
        }
    }
    fn streams(&mut self, flags: u32) -> utility::Streams {
        if let Some(io) = &self.io {
            if flags == 0 { return io.streams.clone(); }
        }
        let input = core::mem::take(&mut self.stream);
        utility::Streams::new((flags & 1 != 0).then_some(input), flags & 2 != 0)
    }
    fn exec(&mut self, req: Spawn, image: &[u8]) -> Result<u8> {
        let mut args = req.args;
        if let Some(handler) = system_handler(image) {
            args[0] = handler.into();
            if let Some(exe) = crate::utility::installed(handler) {
                return self.utility(exe, &args, req.flags);
            }
            if self.io.as_ref().is_some_and(|io| io.streams.redirected())
                && !matches!(handler, "mkdir" | "touch" | "rm") {
                return Err("this command requires a foreground terminal");
            }
            self.stream.clear();
            return self.services.legacy(handler, &args);
        }
        self.utility(image, &args, req.flags)
    }
    fn utility(&mut self, image: &[u8], args: &[String], flags: u32) -> Result<u8> {
        let streams = self.streams(flags);
        let code = utility::segment(image).ok_or("invalid LixinOS executable")?;
        let io = utility::Context::new(&self.services.cwd, self.services.volume.clone(), streams.clone());
        // Standalone utilities run to completion before the caller's bank resumes.
        #[cfg(feature = "retina")]
        let io = { let mut io = io; io.allow_canvas = true; io };
        let packed = pack_args(&args[1..]);
        let (io, status) = io.enter(|| user::launch_status(code, Some((&self.services.cwd, &packed)), false));
        #[cfg(feature="retina")]
        if status == Ok(256) && io.canvas.is_some() {
            return desktop::resume(self, image, io);
        }
        drop(io);
        let status = status.and_then(|v| u8::try_from(v).map_err(|_| "invalid utility exit status"));
        if flags & 2 != 0 { self.stream = streams.output()?; }
        status
    }
    fn request(&mut self, op: u32, index: usize, arg: &str, extra: &[u8]) -> Result<Reply> {
        let services = &mut self.services;
        let text = match op {
            abi::STAT => {
                let path = services.path(arg)?;
                let v = services.volume.borrow();
                let kind = match v.files.get(&path).ok_or("file not found")? {
                    None => "d".into(),
                    Some(bytes) => match system_handler(bytes) {
                        Some(cmd) if crate::utility::installed(cmd).is_none() => format!("@{cmd}"),
                        _ if runnable(bytes) => "x".into(),
                        _ => "f".into(),
                    },
                };
                kind
            }
            abi::HISTORY_GET => self.history.get(index).cloned().ok_or("end of history")?,
            abi::READ_DIRECTORY => {
                let dir = services.path(arg)?;
                let mut parts = extra.split(|c| *c == 0);
                let mut field = || {
                    parts
                        .next()
                        .and_then(|b| core::str::from_utf8(b).ok())
                        .filter(|s| s.len() <= 512 && !s.contains('/'))
                        .ok_or("invalid directory query")
                };
                let (prefix, after) = (field()?, field()?);
                let base = format!("{}/", dir.trim_end_matches('/'));
                let prefix = format!("{base}{prefix}");
                use core::ops::Bound;
                let start = if after.is_empty() {
                    Bound::Included(prefix.clone())
                } else {
                    Bound::Excluded(format!("{base}{after}"))
                };
                services
                    .volume
                    .borrow()
                    .files
                    .range((start, Bound::Unbounded))
                    .take_while(|(p, _)| p.starts_with(&prefix))
                    .find_map(|(p, data)| {
                        let name = p.strip_prefix(&base)?;
                        if name.is_empty() || name.contains('/') {
                            return None;
                        }
                        let kind = match data {
                            None => 1,
                            Some(b) if runnable(b) => 2,
                            _ => 0,
                        };
                        Some(format!("{kind}{name}"))
                    })
                    .ok_or("end of directory")?
            }
            abi::HISTORY_APPEND if !arg.is_empty() && arg.len() <= 512 => {
                services.volume.borrow_mut().disk.append_history(arg)?;
                if self.history.len() == HISTORY_LIMIT {
                    self.history.pop_front();
                }
                self.history.push_back(arg.into());
                return Ok(Reply::Number(0));
            }
            abi::SPAWN_BEGIN if index <= 3 => {
                self.pending = Some(Spawn {
                    path: services.path(arg)?,
                    args: Vec::new(),
                    flags: index as u32,
                });
                return Ok(Reply::Number(0));
            }
            abi::SPAWN_ARG => {
                let spawn = self.pending.as_mut().ok_or("no pending process")?;
                if spawn.args.len() >= 512 {
                    return Err("too many arguments");
                }
                spawn.args.push(arg.into());
                return Ok(Reply::Number(0));
            }
            abi::CHILD_STATUS => return self.last.map(|v| Reply::Number(v as u64)),
            abi::READ_STREAM => {
                self.stream = services.file(arg)?;
                return Ok(Reply::Number(0));
            }
            abi::CLEAR_STREAM => {
                self.stream.clear();
                return Ok(Reply::Number(0));
            }
            abi::APPEND_STREAM => {
                if self.stream.len() + arg.len() > FILE_LIMIT {
                    return Err("file too large");
                }
                self.stream.push_str(arg);
                return Ok(Reply::Number(0));
            }
            abi::STORE_STREAM if index <= 1 => {
                let path = services.path(arg)?;
                let mut contents = match services.volume.borrow().files.get(&path) {
                    Some(None) => return Err("is a directory"),
                    Some(Some(bytes)) if index == 1 => bytes.clone(),
                    _ => Vec::new(),
                };
                if contents.len() + self.stream.len() > FILE_LIMIT {
                    return Err("file too large");
                }
                contents.extend_from_slice(self.stream.as_bytes());
                services.store_bytes(path, Some(contents))?;
                self.stream.clear();
                return Ok(Reply::Number(0));
            }
            abi::LAST_ERROR => self.error.into(),
            abi::CHDIR => {
                let path = services.path(arg)?;
                if services.volume.borrow().files.get(&path) != Some(&None) {
                    return Err("directory not found");
                }
                services.cwd = path.clone();
                path
            }
            #[cfg(feature = "ssh-mux")]
            abi::NEXT_TERMINAL => return Ok(Reply::Number(crate::serial_mux::next().into())),
            #[cfg(feature = "ssh-mux")]
            abi::CLOSE_TERMINAL => {
                crate::serial_mux::close();
                return Ok(Reply::Number(0));
            }
            #[cfg(not(feature = "ssh-mux"))]
            abi::NEXT_TERMINAL | abi::CLOSE_TERMINAL => return Err("SSH terminals unavailable"),
            abi::PANE_READY if self.handshake => {
                self.handshake = false;
                #[cfg(feature = "split")]
                crate::panes::switch();
                return Ok(Reply::Number(0));
            }
            _ => return Err("invalid process request"),
        };
        Ok(Reply::Text(text))
    }
}
enum Reply {
    Number(u64),
    Text(String),
}
pub(crate) fn exchange(value: u64) -> u64 {
    let Some(ctx) = (unsafe { (&mut *SLOTS[user::bank()].0.get()).as_mut() }) else {
        return u64::MAX;
    };
    let bytes = unsafe { core::slice::from_raw_parts_mut(user::git_mailbox(), 4097) };
    let Some(n) = bytes.iter().position(|b| *b == 0) else {
        return u64::MAX;
    };
    let Ok(arg) = core::str::from_utf8(&bytes[..n]) else {
        return u64::MAX;
    };
    match ctx.request(value as u32, (value >> 32) as usize, arg, &bytes[n + 1..]) {
        Ok(Reply::Number(v)) => v,
        Ok(Reply::Text(s)) if s.len() <= 4096 => {
            bytes[..s.len()].copy_from_slice(s.as_bytes());
            s.len() as u64
        }
        Err(e) => {
            ctx.error = e;
            u64::MAX
        }
        _ => u64::MAX,
    }
}

struct Parent {
    ctx: Context,
    image: Vec<u8>,
    memory: Vec<u8>,
    capture: bool,
}
pub(super) fn run(
    services: Services,
    history: VecDeque<String>,
    image: Vec<u8>,
    handshake: bool,
) -> Result<u8> {
    drive(Context::new(services, history, handshake), image, None, false, &mut None)
}

fn drive(mut ctx: Context, mut image: Vec<u8>, mut restore: Option<Vec<u8>>, popup: bool,
         suspended: &mut Option<Parent>) -> Result<u8> {
    let mut parents = Vec::<Parent>::new();
    let mut arguments = None;
    if user::is_executable(&image) && restore.is_none() {
        ctx.io.get_or_insert_with(|| utility::Context::new(&ctx.services.cwd, ctx.services.volume.clone(), utility::Streams::default()));
        arguments = Some((1, String::from(if popup {"/bin/bash\0"} else {"/bin/sh\0"})));
    }
    let slot = SLOTS[user::bank()].0.get();
    loop {
        let cwd = ctx.services.cwd.clone();
        let flags = usize::from(ctx.handshake) | (usize::from(!popup && parents.is_empty()) << 1)
            | (usize::from(cfg!(feature = "ssh-mux") && !popup && parents.is_empty()) << 2);
        let io = ctx.io.take();
        unsafe { *slot = Some(ctx); }
        let mut launch = || match (restore.take(), arguments.take()) {
            (Some(mem), _) => user::resume_process(&image, &mem),
            (_, Some((argc, packed))) => user::start_saved(&image, argc, &packed, flags),
            _ => user::start_process(&image, &cwd, flags),
        };
        let (io, result) = match io {
            Some(io) => { let (io, result) = io.enter(launch); (Some(io), result) }
            None => (None, launch()),
        };
        ctx = unsafe { (*slot).take().unwrap() };
        ctx.io = io;
        if popup && parents.is_empty() && ctx.pending.is_none() && result == Ok(256) {
            *suspended = Some(Parent { ctx, image, memory: user::suspend_process(), capture: false });
            return Ok(0);
        }
        if result != Ok(256) || ctx.pending.as_ref().is_none_or(|r| r.args.is_empty()) {
            let result = result.and_then(|v| u8::try_from(v).map_err(|_| "invalid spawn request"));
            let Some(mut parent) = parents.pop() else {
                return result;
            };
            parent.ctx.last = if parent.capture {
                ctx.io.as_ref().unwrap().streams.output().and_then(|out| {
                    parent.ctx.stream = out;
                    result
                })
            } else { result };
            ctx = parent.ctx;
            image = parent.image;
            restore = Some(parent.memory);
            continue;
        }
        let Some(req) = ctx.pending.take().filter(|r| !r.args.is_empty()) else {
            return Err("invalid spawn request");
        };
        let memory = user::suspend_process();
        match ctx.services.executable_bytes(&req.path) {
            Ok(child) if user::process_image(&child).is_some() || user::is_executable(&child) => {
                let saved = user::is_executable(&child);
                let redirected = ctx.io.as_ref().is_some_and(|io| io.streams.redirected());
                if !saved && (req.flags != 0 || req.args.len() != 1 || redirected) {
                    ctx.last = Err("process requires an interactive terminal");
                } else if parents.len() >= 8 {
                    ctx.last = Err("process nesting limit");
                } else {
                    let mut sub = Context::new(ctx.services.clone(), ctx.history.clone(), false);
                    if saved {
                        let streams = ctx.streams(req.flags);
                        sub.io = Some(utility::Context::new(&sub.services.cwd, sub.services.volume.clone(), streams));
                        arguments = Some((req.args.len(), pack_args(&req.args)));
                    }
                    parents.push(Parent { ctx, image, memory, capture: req.flags & 2 != 0 });
                    ctx = sub;
                    image = child;
                    continue;
                }
            }
            Ok(child) => ctx.last = ctx.exec(req, &child),
            Err(e) => ctx.last = Err(e),
        }
        restore = Some(memory);
    }
}
