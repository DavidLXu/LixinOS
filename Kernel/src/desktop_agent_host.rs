//! Kernel adapters for the desktop workbench's user-space worker.
use super::*;

const PYTHON: &str = "/bin/python3";
const WORKBENCH: &str = "/src/agent/desktop.py";
const WORKER: &str = "/src/agent/native.py";
const TERMINAL: u8 = 1;
const QUIET: u8 = 2;
const MAX_REQUEST: usize = 512 * 1024;
static WORKERS: [AtomicU8; user::BANKS] = [const { AtomicU8::new(0) }; user::BANKS];

struct Worker(usize);
impl Drop for Worker {
    fn drop(&mut self) {
        WORKERS[self.0].store(0, Ordering::Relaxed);
    }
}

pub(crate) fn quiet_desktop_worker() -> bool {
    let mut bank = user::bank();
    loop {
        if WORKERS[bank].load(Ordering::Relaxed) == QUIET { return true; }
        let Some(parent) = crate::scheduler::parent_slot(bank) else { return false; };
        bank = parent;
    }
}

fn operator(terminal: bool) -> u8 {
    if crate::scheduler::stopped() != 0 {
        return 125;
    }
    if terminal {
        match crate::poll_byte() {
            Some(3 | 7) => return 125,
            Some(16) => return 126,
            _ => (),
        }
    }
    0
}

impl Context {
    pub(super) fn desktop_worker(&mut self, args: Vec<String>) -> Result<u8> {
        let quiet = args[1] == "desktop-work";
        if agent_noninteractive() || !quiet && crate::scheduler::background() {
            return Err("desktop agent requires an operator or the desktop client");
        }
        let _worker = Worker(user::bank());
        WORKERS[user::bank()].store(if quiet { QUIET } else { TERMINAL }, Ordering::Relaxed);
        #[cfg(feature = "retina")]
        let _bar = if quiet || args.get(2).is_some_and(|arg| args[1] == "desktop" && arg == "new") {
            None
        } else {
            let root = args.iter().skip(2).find(|arg| arg.as_str() != "--auto-approve")
                .and_then(|arg| self.services.path(arg).ok()).unwrap_or_else(|| self.services.cwd.clone());
            crate::graphics::pin_bar(crate::graphics::TopBar {
                title: " LixinOS Agent ", short_title: " Agent ", context: root,
                badge: " Bypass on ", badge_background: 0xffdf9e,
                status: "Ready", detail: "Desktop".into(),
            })
        };
        let argv = [PYTHON, "-I", WORKER].into_iter().map(String::from)
            .chain(args.into_iter().skip(1)).collect::<Vec<_>>();
        let image = self.services.executable_bytes(PYTHON)?;
        self.utility(&image, &argv)
    }

    pub(super) fn desktop_helper(
        &mut self, path: &str, handler: &str, args: &[String],
    ) -> Result<u8> {
        let Some(parent) = crate::scheduler::parent_slot(user::bank()) else { return Ok(1); };
        let mode = WORKERS[parent].load(Ordering::Relaxed);
        if mode == 0 || !matches!((handler, path),
            ("agent-io", "/lib/agent-io") | ("agent-command", "/lib/agent-command"))
        {
            return Ok(1);
        }
        let io = self.io.take().ok_or("missing desktop agent streams")?;
        let streams = io.streams;
        let (Some(input), Some(output)) = (&streams.input_pipe, &streams.output_pipe) else {
            return Ok(1);
        };
        let terminal = mode == TERMINAL;
        if handler == "agent-command" {
            return self.workbench_command(&args[1..], streams, terminal);
        }
        let [_, action] = args else { return Ok(1); };
        let mut bytes = Vec::new();
        let mut chunk = [0; 4096];
        loop {
            let n = input.read_into(&mut chunk);
            if n == 0 { break; }
            if bytes.len() + n >= MAX_REQUEST { return Ok(1); }
            bytes.extend_from_slice(&chunk[..n]);
        }
        if action == "header" {
            #[cfg(feature = "retina")]
            if terminal && let Some(mode @ (1 | 2)) = bytes.first() {
                crate::graphics::update_bar(|bar| {
                    bar.badge = if *mode == 1 { " Bypass on " } else { " Bypass off " };
                    bar.badge_background = if *mode == 1 { 0xffdf9e } else { 0xbde5ba };
                });
            }
            return Ok(0);
        }
        if action != "model" { return Ok(1); }
        let mut parts = bytes.split(|&byte| byte == b'\t');
        if parts.next() != Some(b"chat-session") { return Ok(1); }
        let fields = parts.map(crate::agent::decode).collect::<Result<Vec<_>>>()?;
        if !(8..=68).contains(&fields.len()) { return Ok(1); }
        let fields = fields.iter().map(String::as_str).collect::<Vec<_>>();
        let mut stopped = 0;
        let result = self.services.with_agent(|bridge| {
            let reply = bridge.request_interruptible("chat-session", &fields, || {
                stopped = operator(terminal);
                if stopped == 0 { Ok(()) } else { Err("desktop request interrupted") }
            });
            Ok(match reply {
                Ok(values) => wire("ok", values.iter().map(String::as_str)),
                Err(error) => match bridge.take_model_error() {
                    Some(model) if model.retryable => wire("error", [model.detail.as_str(), "retryable"]),
                    Some(model) => wire("error", [model.detail.as_str()]),
                    None => wire("error", [error]),
                },
            })
        });
        if stopped != 0 { return Ok(stopped); }
        let reply = result.unwrap_or_else(|error| wire("error", [error]));
        let mut rest = reply.as_bytes();
        while !rest.is_empty() {
            let Some(n) = output.write_from(rest) else { return Ok(1); };
            rest = &rest[n..];
        }
        Ok(0)
    }

    fn workbench_command(
        &self, args: &[String], streams: utility::Streams, terminal: bool,
    ) -> Result<u8> {
        let [python, isolated, script, op, operands @ ..] = args else { return Ok(1); };
        if python != PYTHON || isolated != "-I" || script != WORKBENCH
            || !match (op.as_str(), operands) {
                ("read", [_] | [_, _, _]) | ("write", [_, _]) | ("patch", [_, _, _])
                | ("status" | "check", []) => true,
                ("check", [flag]) => flag == "--reuse",
                _ => false,
            } || args.iter().map(|arg| arg.len() + 1).sum::<usize>() > 4096
        {
            return Ok(1);
        }
        let root = &self.services.cwd;
        if matches!(root.split('/').nth(1), None | Some("" | "bin" | "lib" | "libexec" | "src" | "boot" | "agent-policy" | "proc" | "sys"))
            || root.split('/').any(|part| part.eq_ignore_ascii_case(".git"))
        {
            return Ok(1);
        }
        let services = self.services.clone();
        let image = services.executable_bytes(PYTHON)?;
        let request = Spawn { path: PYTHON.into(), args: args.to_vec(), flags: 0 };
        let cancelled = Rc::new(Cell::new(false));
        let child_cancelled = cancelled.clone();
        let pid = crate::scheduler::spawn(alloc::boxed::Box::new(move || {
            let slot = AGENT_CONSOLES[user::bank()].0.get();
            unsafe {
                *slot = Some(AgentConsole {
                    output: Vec::new(), truncated: false, console_lines: 0,
                    console_truncated: false, last_shown: None,
                    deadline: crate::monitor::ticks() + crate::monitor::seconds_to_ticks(60),
                    cancelled: child_cancelled, timed_out: false, needs_terminal: false,
                    workspace: Some(services.cwd.clone()),
                });
                ACTIVE_AGENT_CONSOLES += 1;
            }
            let status = run_child(services, VecDeque::new(), request, image, streams);
            let console = unsafe {
                ACTIVE_AGENT_CONSOLES -= 1;
                (*slot).take().unwrap()
            };
            if console.timed_out { Ok(124) } else { status }
        }), crate::scheduler::pane())?;
        let mut interrupted = 0;
        let mut status = loop {
            if let Some(status) = crate::scheduler::try_wait(pid) { break status; }
            if interrupted == 0 {
                interrupted = operator(terminal);
                if interrupted != 0 { cancelled.set(true); }
            }
            crate::scheduler::sleep_ms(1);
        };
        if interrupted != 0 { status = Ok(interrupted); }
        status
    }
}

fn wire<'a>(status: &str, fields: impl IntoIterator<Item = &'a str>) -> String {
    let mut reply = String::from(status);
    for field in fields {
        reply.push('\t');
        for byte in field.bytes() {
            write!(reply, "{byte:02x}").unwrap();
        }
    }
    reply
}
