//! Process launch, wait, and pipe attachment for disk-resident shells and their children.
#[cfg(feature="retina")]
#[path="desktop.rs"]
mod desktop;
#[path = "../../User/sh/abi.rs"]
mod abi;
use super::*;
use crate::agent_tools::{COLOR_RESET, PREVIEW_LINES, PREVIEW_OMITTED, ToolReport, ToolStatus, preview_lines, print_tool};
use crate::{user, utility};
use core::{cell::{Cell, UnsafeCell}, fmt::Write, sync::atomic::{AtomicU8, Ordering}};

#[path = "desktop_agent_host.rs"]
mod desktop_agent_host;
pub(crate) use desktop_agent_host::quiet_desktop_worker;

const AGENT_OUTPUT_LIMIT: usize = 1536;
const AGENT_ARGUMENT_PREVIEW_BYTES: usize = 240;
const SSH_HANDOFF: &str = "SSH sessions require the agent's terminal tool with SSH arguments";

fn ssh_needs_terminal(path: &str, image: &[u8], args: &[String]) -> bool {
    let is_ssh = path == "/bin/ssh"
        || system_handler(image) == Some("ssh")
        || utility::installed("ssh").is_some_and(|stock| image == stock);
    is_ssh && !args.is_empty() && !matches!(args, [flag] if flag == "--help")
        && !matches!(args, [flag, profile] if flag == "--public-key" && profile == "gpu20")
}

struct AgentConsole {
    output: Vec<u8>,
    truncated: bool,
    console_lines: usize,
    console_truncated: bool,
    last_shown: Option<u8>,
    deadline: u64,
    cancelled: Rc<Cell<bool>>,
    timed_out: bool,
    needs_terminal: bool,
    workspace: Option<String>,
}
struct AgentSlot(UnsafeCell<Option<AgentConsole>>);
unsafe impl Sync for AgentSlot {}
static AGENT_CONSOLES: [AgentSlot; user::BANKS] =
    [const { AgentSlot(UnsafeCell::new(None)) }; user::BANKS];
static mut ACTIVE_AGENT_CONSOLES: usize = 0;

fn agent_console_slot() -> Option<*mut Option<AgentConsole>> {
    if unsafe { ACTIVE_AGENT_CONSOLES == 0 } { return None; }
    let mut bank = user::bank();
    loop {
        let slot = AGENT_CONSOLES[bank].0.get();
        if unsafe { (*slot).is_some() } { return Some(slot); }
        bank = crate::scheduler::parent_slot(bank)?;
    }
}

fn agent_automatic(services: &Services, path: &str, args: &[String], image: &[u8]) -> bool {
    if !path.strip_prefix("/bin/").and_then(utility::installed).is_some_and(|stock| image == stock) {
        return false;
    }
    match (path, args) {
        ("/bin/pwd" | "/bin/cpuinfo" | "/bin/sysinfo" | "/bin/uname" | "/bin/ps" | "/bin/df", []) => true,
        ("/bin/uname", [option]) => matches!(option.as_str(), "-a" | "-s" | "-m" | "-r"),
        ("/bin/ls" | "/bin/cat", operands) => {
            (path == "/bin/ls" || !operands.is_empty()) && operands.iter().all(|arg| {
                (path == "/bin/ls" && matches!(arg.as_str(), "-1" | "-l" | "-h" | "-lh" | "-a" | "-lah"))
                    || (!arg.starts_with('-') && services.path(arg).is_ok_and(|resolved| {
                        if path == "/bin/cat" {
                            let guide: Option<&[u8]> = match resolved.as_str() {
                                "/src/build/README.md" => Some(include_bytes!("../../User/build/README.md")),
                                "/src/tinycc/README.md" => Some(include_bytes!("../../User/tinycc/README.md")),
                                _ => None,
                            };
                            if guide.is_some_and(|stock| {
                                services.volume.borrow().files.get(&resolved).and_then(Option::as_deref) == Some(stock)
                            }) {
                                return true;
                            }
                        }
                        if crate::procfs::contains(&resolved) {
                            let volume = services.volume.borrow();
                            if volume.is_virtual_proc(&resolved) {
                                return crate::procfs::file(&resolved).is_some()
                                    || (path == "/bin/ls" && resolved == crate::procfs::ROOT);
                            }
                        }
                        let cwd = &services.cwd;
                        (resolved == *cwd || resolved.strip_prefix(cwd).is_some_and(|tail| tail.starts_with('/')))
                            && !resolved.split('/').any(|part| part.eq_ignore_ascii_case(".git"))
                    }))
            })
        }
        _ => false,
    }
}

// Capture tool output for the model and return whether to display this byte.
pub(crate) fn capture_agent_output(byte: u8) -> bool {
    if let Some(slot) = agent_console_slot()
        && let Some(console) = unsafe { (*slot).as_mut() }
    {
        if console.output.len() < AGENT_OUTPUT_LIMIT {
            console.output.push(byte);
        } else {
            console.truncated = true;
        }
        if console.console_lines == PREVIEW_LINES {
            console.console_truncated = true;
            return false;
        }
        console.console_lines += usize::from(byte == b'\n');
        console.last_shown = Some(byte);
    }
    true
}

// Use normal process loading with an explicit argv. Terminal handoff and
// captured commands have different input and output handling.
pub(crate) fn agent_command(
    cwd: &str,
    volume: Rc<RefCell<Volume>>,
    started: u64,
    args: Vec<String>,
    interactive: bool,
    scoped: bool,
    progress: (usize, usize),
    approve: impl FnOnce(Option<&str>) -> Result<bool>,
) -> Result<ToolReport> {
    let services = Services {
        agent_bridge: model_bridge(),
        volume,
        started,
        cwd: cwd.into(),
    };
    let name = &args[0];
    let path = if name.contains('/') { services.path(name)? } else { format!("/bin/{name}") };
    let (step, limit) = progress;
    let kind = if interactive { "terminal handoff" } else { "exec" };
    let mut heading = format!("{kind} in {cwd:?}: {path:?}");
    let mut shortened = false;
    for arg in &args[1..] {
        let prefix = preview_lines(arg);
        let mut end = prefix.len().min(AGENT_ARGUMENT_PREVIEW_BYTES);
        while !prefix.is_char_boundary(end) { end -= 1; }
        write!(heading, " {:?}", &prefix[..end]).unwrap();
        if end < arg.len() {
            shortened = true;
            heading.push_str(" ...");
        }
    }
    print_tool(step, limit, &heading);
    let image = services.executable_bytes(&path)?;
    if !runnable(&image) {
        return Err("not a LixinOS executable");
    }
    let handler = system_handler(&image);
    if !interactive && ssh_needs_terminal(&path, &image, &args[1..]) {
        print(&format!("agent: command failed: {SSH_HANDOFF}\n"));
        return Ok(ToolReport::new(ToolStatus::RequiresTerminal, SSH_HANDOFF));
    }
    if !interactive && matches!(handler, Some("shutdown" | "agent" | "nano" | "kilo" | "desktop" | "paint" | "htop")) {
        let detail = "this command requires terminal handoff";
        print(&format!("agent: command failed: {detail}\n"));
        return Ok(ToolReport::new(ToolStatus::RequiresTerminal, detail));
    }
    let automatic = !interactive && agent_automatic(&services, &path, &args[1..], &image);
    if !automatic {
        let full = shortened.then(|| format!("{path:?} {:?}", &args[1..]));
        if !approve(full.as_deref())? {
            return Ok(ToolReport::new(ToolStatus::Declined, "Command declined by operator. It did not run."));
        }
    }
    let req = Spawn { path, args, flags: 0 };
    if crate::scheduler::background() { return Err("agent commands require a foreground session"); }
    let result = Rc::new(RefCell::new(None));
    let child_result = result.clone();
    let cancelled = Rc::new(Cell::new(false));
    let child_cancelled = cancelled.clone();
    let pid = crate::scheduler::spawn(alloc::boxed::Box::new(move || {
        let streams = utility::Streams::default();
        let output = AGENT_CONSOLES[user::bank()].0.get();
        if !interactive {
            let deadline = crate::monitor::ticks() + crate::monitor::seconds_to_ticks(60);
            unsafe {
                *output = Some(AgentConsole {
                    output: Vec::new(), truncated: false, console_lines: 0, console_truncated: false,
                    last_shown: None, deadline, cancelled: child_cancelled,
                    timed_out: false, needs_terminal: false,
                    workspace: scoped.then(|| services.cwd.clone()),
                });
                ACTIVE_AGENT_CONSOLES += 1;
            }
        }
        let status = run_child(services, VecDeque::new(), req, image, streams);
        let console = unsafe { (*output).take() };
        if console.is_some() { unsafe { ACTIVE_AGENT_CONSOLES -= 1; } }
        *child_result.borrow_mut() = console;
        status
    }), crate::scheduler::pane())?;
    let status = if interactive {
        crate::scheduler::wait(pid)
    } else {
        loop {
            if let Some(status) = crate::scheduler::try_wait(pid) { break status; }
            if crate::scheduler::stopped() != 0 || matches!(crate::poll_byte(), Some(3 | 7)) {
                cancelled.set(true);
            }
            crate::scheduler::sleep_ms(1);
        }
    };
    print(COLOR_RESET);
    if cancelled.get() {
        return Err(crate::agent_tools::CANCELLED);
    }
    let console = result.borrow_mut().take();
    if let Some(console) = &console {
        if console.console_truncated {
            print(PREVIEW_OMITTED);
            print("\n");
        } else if console.last_shown.is_some_and(|byte| byte != b'\n') {
            print("\n");
        }
    } else if interactive {
        print("\n");
    }
    if console.as_ref().is_some_and(|console| console.timed_out) {
        let detail = "Command timed out after 60 seconds.";
        print(&format!("agent: {detail}\n"));
        return Ok(ToolReport::new(ToolStatus::TimedOut, detail));
    }
    let status = match status {
        Ok(status) => status,
        Err(error) => {
            print(&format!("agent: command failed: {error}\n"));
            return Ok(ToolReport::new(ToolStatus::Failed, format!("Command failed: {error}.")));
        }
    };
    if status != 0 {
        print(&format!("agent: command exited with status {status}\n"));
    }
    let mut summary = format!("Command exited with status {status}.");
    let needs_terminal = console.as_ref().is_some_and(|console| console.needs_terminal);
    if let Some(console) = console {
        if !console.output.is_empty() {
            summary.push_str("\n");
            summary.push_str(&String::from_utf8_lossy(&console.output));
        }
        if console.truncated { summary.push_str("\n[output preview truncated]"); }
    }
    let outcome = if needs_terminal {
        ToolStatus::RequiresTerminal
    } else if status == 0 {
        ToolStatus::Ok
    } else {
        ToolStatus::Failed
    };
    let mut report = ToolReport::new(outcome, summary);
    report.exit_code = Some(status);
    Ok(report)
}

pub(crate) fn agent_write_allowed(path: &str) -> bool {
    let Some(slot) = agent_console_slot() else { return true; };
    let console = unsafe { (*slot).as_ref().unwrap() };
    console.workspace.as_deref().is_none_or(|root|
        path.strip_prefix(root).is_some_and(|tail| tail.starts_with('/') && !tail.split('/').any(|part| part == ".git")))
}

pub(crate) fn agent_noninteractive() -> bool {
    agent_console_slot().is_some()
}

// os.system() enters the same guest Bash and child path as the terminal.
// The command is copied out of the caller's mailbox before a child runs.
pub(crate) fn spawn_program(cwd: String, volume: Rc<RefCell<Volume>>, streams: utility::Streams,
    path: String, args: Vec<String>) -> Result<usize> {
    let services = Services { cwd, volume, started: crate::monitor::ticks(),
        agent_bridge: model_bridge() };
    let image = services.executable_bytes(&path)?;
    let req = Spawn { path, args, flags: 0 };
    crate::scheduler::spawn(alloc::boxed::Box::new(move || {
        // The child has a PID even if its executable handler rejects the request.
        // Its parent must be able to reap that PID and observe a command failure.
        Ok(run_child(services, VecDeque::new(), req, image, streams).unwrap_or(126))
    }), crate::scheduler::pane())
}

pub(crate) fn python_system() -> u64 {
    let run = || -> Result<u8> {
        let Some((cwd, volume, streams)) = utility::command_context() else {
            return Err("system command requires a process context");
        };
        let bytes = unsafe { core::slice::from_raw_parts(user::utility_mailbox(), 513) };
        let len = bytes.iter().position(|&byte| byte == 0).ok_or("command too long")?;
        let command = core::str::from_utf8(&bytes[..len]).map_err(|_| "invalid command")?;
        let services = Services { cwd, volume, started: crate::monitor::ticks(),
            agent_bridge: model_bridge() };
        let path = String::from("/bin/bash");
        let image = services.executable_bytes(&path)?;
        if !user::is_executable(&image) { return Err("expected /bin/bash in LXE format"); }
        let args = alloc::vec![path.clone(), String::from("-c"), command.into()];
        let req = Spawn { path, args, flags: 0 };
        let child = crate::scheduler::spawn(alloc::boxed::Box::new(move ||
            run_child(services, VecDeque::new(), req, image, streams)
        ), crate::scheduler::pane())?;
        crate::scheduler::wait(child)
    };
    run().map_or(u64::MAX, u64::from)
}

#[unsafe(no_mangle)]
pub(crate) extern "C" fn user_interrupt_status() -> u64 {
    let stop = crate::scheduler::stopped();
    if stop != 0 { return stop.into(); }
    let expired = agent_console_slot().is_some_and(|slot| unsafe {
        (*slot).as_mut().is_some_and(|console| {
            console.timed_out |= crate::monitor::ticks() >= console.deadline;
            console.cancelled.get() || console.timed_out
        })
    });
    if expired { 124 } else { 0 }
}

struct Spawn {
    path: String,
    args: Vec<String>,
    flags: u32,
}
struct Context {
    services: Services,
    history: VecDeque<String>,
    pending: Option<Spawn>,
    pipe: Option<alloc::rc::Rc<crate::pipe::Reader>>,
    out_file: Option<(String,bool)>,
    in_file:Option<Vec<u8>>,
    stream: String,
    last: Result<u8>,
    error: &'static str,
    handshake: bool,
    login: bool,
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
        crate::scheduler::ready();
    }
}
impl Context {
    fn new(services: Services, history: VecDeque<String>, handshake: bool) -> Self {
        Self {
            services,
            history,
            handshake,
            login: false,
            io: None,
            pending: None,
            pipe: None,
            out_file: None,
            in_file:None,
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
            if matches!(handler, "agent-io" | "agent-command") {
                return self.desktop_helper(&req.path, handler, &args);
            }
            if handler == "agent" && req.path == "/bin/agent"
                && args.get(1).is_some_and(|mode| matches!(mode.as_str(), "desktop" | "desktop-work" | "resume"))
            {
                return self.desktop_worker(args);
            }
            args[0] = handler.into();
            if let Some(exe) = crate::utility::installed(handler) {
                return self.utility(exe, &args);
            }
            if handler == "python" {
                let io = self.io.take().ok_or("missing process descriptors")?;
                let script = match &args[..] {
                    [_] => None,
                    [_, flag, source] if flag == "-c" => Some(("<string>", source.clone())),
                    [_, path] => Some((path.as_str(), self.services.file(path)?)),
                    _ => return Err("usage: python [script.py | -c CODE]"),
                };
                let (_, result) = io.enter(|| user::launch(
                    user::PYTHON, script.as_ref().map(|(p, s)| (*p, s.as_str())), false,
                ));
                return result;
            }
            if handler == "tcc" && agent_noninteractive()
                && let [_, src, flag, dst] = &args[..] && flag == "-o"
                && self.io.as_ref().is_some_and(|io| io.streams.output_pipe.is_some())
            {
                let streams = self.io.take().unwrap().streams;
                let errors = streams.error_pipe.clone().or_else(|| streams.output_pipe.clone());
                if let Err(error) = self.services.compile_lxe(src, dst, streams) {
                    if let Some(errors) = errors {
                        let detail = format!("tcc: error: {error}\n");
                        let mut rest = detail.as_bytes();
                        while !rest.is_empty() {
                            let Some(n) = errors.write_from(rest) else { break; };
                            rest = &rest[n..];
                        }
                    }
                    return Ok(1);
                }
                return Ok(0);
            }
            if self.io.as_ref().is_some_and(|io| io.streams.redirected())
                && !(crate::scheduler::background() && handler=="tcc")
            {
                return Err("this command requires a foreground terminal");
            }
            self.stream.clear();
            return self.services.legacy(handler, &args);
        }
        self.utility(image, &args)
    }
    fn utility(&mut self, image: &[u8], args: &[String]) -> Result<u8> {
        let io = self.io.take().ok_or("missing process descriptors")?;
        let packed = pack_args(&args[1..]);
        let (io, status) = io.enter(|| utility::launch_program(image, &self.services.cwd, &packed));
        #[cfg(feature="retina")]
        if status == Ok(256) && io.canvas.is_some() {
            return desktop::resume(self, image, io);
        }
        drop(io);
        let status = status.and_then(|v| u8::try_from(v).map_err(|_| "invalid utility exit status"));
        status
    }
    fn start_job(&mut self, flags: usize) -> Result<usize> {
        if flags & 4 != 0 && agent_noninteractive() {
            return Err("background jobs require terminal handoff");
        }
        let req = self
            .pending
            .take()
            .filter(|r| !r.args.is_empty())
            .ok_or("missing command")?;
        let image = self.services.executable_bytes(&req.path)?;
        let detached = flags & 4 != 0;
        let next_pipe = flags & 2 != 0;
        let file = self.out_file.take();
        let mut streams = utility::Streams::from_bytes(
            self.in_file
                .take()
                .or_else(|| (detached && self.pipe.is_none()).then(Vec::new)),
            file.is_some(),
        );
        if let Some((_, _, parent)) = utility::command_context() { streams.environment = parent.environment; }
        let incoming = self.pipe.take();
        if req.flags & 1 == 0 {
            streams.input_pipe = incoming;
        }
        if flags == 8 {
            streams = self.streams(0);
        }
        if next_pipe {
            let (reader, writer) = crate::pipe::pair();
            self.pipe = Some(reader);
            if file.is_none() {
                streams.output_pipe = Some(writer);
            }
        }
        let mut services = self.services.clone();
        let history = self.history.clone();
        let run = move || -> Result<u8> {
            let captured = file.as_ref().map(|_| streams.capture_view());
            let status = run_child(services.clone(), history, req, image, streams);
            if let Some((path, append)) = file {
                let path = services.path(&path)?;
                if services.volume.borrow().files.get(&path) == Some(&None) {
                    return Err("is a directory");
                }
                let output = captured.unwrap().output_bytes()?;
                let bytes = if append {
                    let volume = services.volume.borrow();
                    let current = volume.files.get(&path).and_then(Option::as_deref).unwrap_or_default();
                    if current.len() + output.len() > FILE_LIMIT { return Err("file too large"); }
                    let mut combined = Vec::new();
                    combined.try_reserve_exact(current.len() + output.len())
                        .map_err(|_| "not enough file buffer memory")?;
                    combined.extend_from_slice(current);
                    combined.extend_from_slice(&output);
                    combined
                } else {
                    output
                };
                services.store_bytes(path, Some(bytes))?;
            }
            status
        };
        let id = crate::scheduler::spawn(alloc::boxed::Box::new(run), crate::scheduler::pane())?;
        if detached {
            crate::scheduler::detach(id);
        } else {
            crate::scheduler::job(id);
        }
        Ok(id)
    }

    fn request(&mut self, op: u32, index: usize, arg: &str, extra: &[u8]) -> Result<Reply> {
        let services = &mut self.services;
        let text = match op {
            abi::STAT => {
                let path = services.path(arg)?;
                let v = services.volume.borrow();
                let kind = match v.entry(&path).ok_or("file not found")? {
                    Entry::Directory => "d".into(),
                    Entry::ProcFile(_) => "f".into(),
                    Entry::File(bytes) => match system_handler(bytes) {
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
                let volume = services.volume.borrow();
                let (name, entry) = volume.children(&dir, prefix, (!after.is_empty()).then_some(after))
                    .next().ok_or("end of directory")?;
                let kind = match entry {
                    Entry::Directory => 1,
                    Entry::File(bytes) if runnable(bytes) => 2,
                    _ => 0,
                };
                format!("{kind}{name}")
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
                #[cfg(feature = "retina")]
                if utility::owns_canvas() {
                    return Err("cannot launch a child while drawing on the canvas");
                }
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
            abi::START_JOB => return self.start_job(index).map(|id|Reply::Number(id as u64)),
            abi::WAIT_JOB => return crate::scheduler::wait(index).map(|code|Reply::Number(code.into())),
            abi::RESET_PIPE => {self.pipe=None;self.out_file=None;self.in_file=None;return Ok(Reply::Number(0));}
            abi::JOB_INPUT=>{self.in_file=Some(services.bytes(arg)?);return Ok(Reply::Number(0));}
            abi::PID=>return Ok(Reply::Number(crate::scheduler::pid() as u64)),
            abi::JOB_OUTPUT => {
                let path=services.path(arg)?;
                let volume = services.volume.borrow();
                volume.require_writable(&path)?;
                if volume.files.get(&path)==Some(&None){return Err("is a directory");}
                self.out_file=Some((path,index!=0));
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
                    Some(Some(bytes)) if index == 1 => bytes.to_vec(),
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
                if !matches!(services.volume.borrow().entry(&path), Some(Entry::Directory)) {
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
                crate::scheduler::ready();
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

pub(super) fn run(
    services: Services,
    history: VecDeque<String>,
    image: Vec<u8>,
    handshake: bool,
) -> Result<u8> {
    let mut ctx = Context::new(services, history, handshake);
    ctx.login = true;
    drive(ctx, image, false, None)
}

fn run_child(
    services: Services,
    history: VecDeque<String>,
    req: Spawn,
    image: Vec<u8>,
    streams: utility::Streams,
) -> Result<u8> {
    if let Some(slot) = agent_console_slot()
        && ssh_needs_terminal(&req.path, &image, &req.args[1..])
    {
        unsafe { (*slot).as_mut().unwrap().needs_terminal = true; }
        return Err(SSH_HANDOFF);
    }
    let mut ctx = Context::new(services, history, false);
    let mut io = utility::Context::new(&ctx.services.cwd, ctx.services.volume.clone(), streams);
    io.executable = ctx.services.path(&req.path)?;
    #[cfg(feature = "retina")]
    let io = {
        let mut io = io;
        io.allow_canvas = !crate::scheduler::background() && !agent_noninteractive() && !crate::graphics::popup_active() && crate::graphics::tab_input_allowed();
        io
    };
    let saved = user::is_executable(&image);
    if !saved
        && user::process_image(&image).is_some()
        && (req.args.len() != 1 || io.streams.redirected())
    {
        return Err("process requires an interactive terminal");
    }
    ctx.io = Some(io);
    if saved || user::process_image(&image).is_some() {
        let args = saved.then(|| (req.args.len(), pack_args(&req.args)));
        drive(ctx, image, false, args)
    } else {
        // Redirections have been inherited by the child's descriptor context.
        ctx.exec(req, &image)
    }
}

fn drive(
    mut ctx: Context,
    image: Vec<u8>,
    popup: bool,
    mut arguments: Option<(usize, String)>,
) -> Result<u8> {
    if arguments.is_none() && user::is_executable(&image) {
        ctx.io.get_or_insert_with(|| {
            utility::Context::new(
                &ctx.services.cwd,
                ctx.services.volume.clone(),
                utility::Streams::default(),
            )
        });
        arguments = Some((
            1,
            String::from(if popup { "/bin/bash\0" } else { "/bin/sh\0" }),
        ));
    }
    let slot = SLOTS[user::bank()].0.get();
    let mut continuing=false;
    loop {
        let cwd = ctx.services.cwd.clone();
        let flags = usize::from(ctx.handshake)
            | (usize::from(ctx.login) << 1)
            | (usize::from(cfg!(feature = "ssh-mux") && ctx.login) << 2);
        let io = ctx.io.take();
        unsafe {
            *slot = Some(ctx);
        }
        let mut launch = || match arguments.take() {
            _ if continuing => user::continue_process(),
            Some((argc, packed)) => user::start_saved(&image, argc, &packed, flags),
            None => user::start_process(&image, &cwd, flags),
        };
        let (io, result) = match io {
            Some(io) => {
                let (io, result) = io.enter(launch);
                (Some(io), result)
            }
            None => (None, launch()),
        };
        ctx = unsafe { (*slot).take().unwrap() };
        ctx.io = io;
        #[cfg(feature = "retina")]
        if popup && crate::graphics::terminal_closed() {
            return Ok(0);
        }
        #[cfg(feature="retina")]
        if !popup && ctx.pending.is_none() && result == Ok(256)
            && ctx.io.as_ref().is_some_and(|io| io.canvas.is_some()) {
            let desktop_io = ctx.io.take().unwrap();
            return desktop::resume(&ctx, &image, desktop_io);
        }
        if result != Ok(256) || ctx.pending.as_ref().is_none_or(|r| r.args.is_empty()) {
            return result.and_then(|v| u8::try_from(v).map_err(|_| "invalid spawn request"));
        }
        let req = ctx.pending.take().unwrap();
        ctx.last = (|| {
            let child = ctx.services.executable_bytes(&req.path)?;
            let streams = ctx.streams(req.flags);
            let capture = (req.flags & 2 != 0).then(|| streams.clone());
            let services = ctx.services.clone();
            let history = ctx.history.clone();
            let child = crate::scheduler::spawn(
                alloc::boxed::Box::new(move || run_child(services, history, req, child, streams)),
                crate::scheduler::pane(),
            )?;
            crate::scheduler::job(child);
            let status = crate::scheduler::wait(child);
            if let Some(streams) = capture {
                ctx.stream = streams.output()?;
            }
            status
        })();
        continuing=true;
    }
}
