#[path = "process_io.rs"]
mod io;

use crate::{
    agent::{self, Revision, Version, Versions},
    print,
};
use alloc::{collections::VecDeque, format, string::String, vec::Vec};
pub(crate) use io::{active, agent_command, agent_noninteractive, agent_write_allowed, capture_agent_output, exchange, python_system, quiet_desktop_worker, spawn_program};
#[cfg(feature = "split")]
pub(crate) use io::ready_on_read;

use alloc::rc::Rc;
use core::cell::RefCell;

type Result<T> = core::result::Result<T, &'static str>;
use crate::disk::{self, ENTRY_LIMIT, FILE_LIMIT, HISTORY_LIMIT};
const COMMANDS: &[&str] = &[
    "agent", "cat", "cd", "cecho", "clear", "desktop", "echo", "git", "grep", "help", "htop", "kilo", "links", "links2", "ls",
    "head", "tail", "tee", "true", "false", "sleep", "date", "sort", "uniq", "find", "ps", "df", "stat", "tree", "du", "cmp", "diff", "nl", "cut", "tr", "sed", "seq", "basename", "dirname", "test", "printf",
    "lust", "nano", "nxeyes", "stopwatch.win2", "netinfo", "cpuinfo", "paint", "pwd", "python", "sftp", "ssh", "shutdown", "sysinfo", "tcc", "uname", "wc",
];

// A system launcher names a handler supplied by the running kernel.
const LAUNCHER_PREFIX: &[u8] = b"LXcmd1\n";

fn system_handler(bytes: &[u8]) -> Option<&'static str> {
    let name = bytes.strip_prefix(LAUNCHER_PREFIX)?;
    match name {
        b"agent-io" => return Some("agent-io"),
        b"agent-command" => return Some("agent-command"),
        _ => (),
    }
    COMMANDS.iter().copied().find(|cmd| cmd.as_bytes() == name)
}

fn old_core_elf(name: &str, sig: (usize, u32)) -> bool {
    if matches!((name, sig.0),
        ("grep" | "find", 12424 | 12648))
    {
        return matches!((name, sig.1),
            ("grep", 0xc9cf_ac87 | 0xa1f8_1447) | ("find", 0x9d0d_3de5 | 0x5df1_2023));
    }
    let (size, sums) = match name {
        "cat" => (5872, [0xbece_70c8, 0xb4d9_aa74]),
        "grep" => (5872, [0x609d_e03c, 0x2c01_ba6b]),
        "wc" => (5880, [0x9647_b6a6, 0x264c_8e3e]),
        _ => return false,
    };
    (sig.0 == size || sig.0 == size + 464) && sums.contains(&sig.1)
}

// A disk-resident login alias follows replacements of /bin/bash. Writing
// a real executable to /bin/sh still explicitly overrides the login program.
const BASH_ALIAS: &[u8] = b"LXalias1\n/bin/bash";

const LINKS_ALIAS: &[u8] = b"LXalias1\n/bin/links";

fn runnable(bytes: &[u8]) -> bool {
    bytes == LINKS_ALIAS || bytes == BASH_ALIAS || system_handler(bytes).is_some()
        || crate::utility::load_segments(bytes).is_some()
        || crate::user::is_executable(bytes)
        || crate::user::process_image(bytes).is_some()
}

pub(crate) struct Volume {
    pub(crate) disk: crate::disk::Disk,
    pub(crate) files: crate::disk::Files,
    pub(crate) versions: Versions,
    agent_trace_writes: u64,
}

pub(crate) enum Entry<'a> {
    Directory,
    File(&'a crate::disk::FileData),
    ProcFile(crate::procfs::File),
}

impl Entry<'_> {
    pub(crate) fn size(&self) -> Option<usize> {
        match self {
            Self::Directory => None,
            Self::File(file) => Some(file.len()),
            Self::ProcFile(_) => Some(0),
        }
    }
}

impl Volume {
    // Trace persistence changes the disk generation but cannot change staged
    // source files, verification evidence or a repeated command's inputs.
    pub(crate) fn agent_revision(&self) -> u64 {
        self.disk.revision().wrapping_sub(self.agent_trace_writes)
    }

    pub(crate) fn commit_agent_trace(&mut self, path: String, bytes: Vec<u8>) -> Result<()> {
        self.commit(path, Some(Some(bytes)), None)?;
        self.agent_trace_writes = self.agent_trace_writes.wrapping_add(1);
        Ok(())
    }

    fn proc_mounted(&self) -> bool {
        !self.files.contains_key(crate::procfs::ROOT)
    }

    pub(crate) fn is_virtual_proc(&self, path: &str) -> bool {
        crate::procfs::contains(path) && self.proc_mounted()
    }

    pub(crate) fn require_writable(&self, path: &str) -> Result<()> {
        if !agent_write_allowed(path) { return Err("write outside the agent project"); }
        if self.is_virtual_proc(path) { Err("read-only filesystem") } else { Ok(()) }
    }

    pub(crate) fn resolve(&self, cwd: &str, name: &str) -> Result<String> {
        disk::resolve_path(cwd, name, |path| matches!(self.entry(path), Some(Entry::Directory)))
    }

    pub(crate) fn entry(&self, path: &str) -> Option<Entry<'_>> {
        if self.is_virtual_proc(path) {
            return if path == crate::procfs::ROOT { Some(Entry::Directory) }
            else { crate::procfs::file(path).map(Entry::ProcFile) };
        }
        self.files.get(path).map(|data| data.as_ref().map_or(Entry::Directory, Entry::File))
    }

    pub(crate) fn read_file(&self, path: &str) -> Result<Vec<u8>> {
        match self.entry(path).ok_or("file not found")? {
            Entry::Directory => Err("is a directory"),
            Entry::File(file) => file.try_as_slice().map(<[u8]>::to_vec),
            Entry::ProcFile(file) => Ok(file.read().into_bytes()),
        }
    }

    pub(crate) fn children(&self, path: &str, prefix: &str, after: Option<&str>)
        -> impl Iterator<Item = (&str, Entry<'_>)> {
        let matches = move |name: &str| name.starts_with(prefix) && after.is_none_or(|key| key < name);
        let mut proc_root = (path == "/" && self.proc_mounted() && matches("proc"))
            .then_some(("proc", Entry::Directory));
        let mut disk = disk::children_matching(&self.files, path, prefix, after)
            .map(|(name, data)| (name, data.as_ref().map_or(Entry::Directory, Entry::File)))
            .peekable();
        let children = core::iter::from_fn(move || {
            if proc_root.is_some() && disk.peek().is_none_or(|(name, _)| "proc" < *name) {
                proc_root.take()
            } else {
                disk.next()
            }
        });
        let proc_files = (path == crate::procfs::ROOT && self.proc_mounted())
            .then_some(crate::procfs::FILES).into_iter().flatten()
            .filter(move |(name, _)| matches(name))
            .map(|&(name, file)| (name, Entry::ProcFile(file)));
        children.chain(proc_files)
    }

    pub(crate) fn save_changes<'a>(
        &mut self,
        changes: impl Iterator<Item = (&'a str, bool)>,
        moved: &disk::Times,
    ) -> Result<()> {
        let now = crate::utility::wall_seconds();
        let mut undo = Vec::with_capacity(changes.size_hint().0);
        let mut parents = alloc::collections::BTreeSet::new();
        let mut scratch_only = true;
        for (path, existed) in changes {
            self.disk.invalidate_body(path);
            scratch_only &= self.disk.scratch_path(path) && !self.versions.contains_key(path);
            let present = self.files.contains_key(path);
            let previous = self.disk.times.get(path).copied();
            undo.push((path, previous));
            if present {
                let time = moved.get(path).copied().unwrap_or_else(|| disk::FileTime {
                    birth: if existed { previous.unwrap_or_default().birth } else { now },
                    modified: now,
                });
                self.set_time(path, Some(time));
            } else {
                self.set_time(path, None);
            }
            if (present != existed || moved.contains_key(path)) && path != "/" {
                let (parent, _) = path.rsplit_once('/').unwrap();
                let parent = if parent.is_empty() { "/" } else { parent };
                if self.files.contains_key(parent) && !moved.contains_key(parent) {
                    parents.insert(parent);
                }
            }
        }
        for parent in parents {
            let previous = self.disk.times.get(parent).copied();
            undo.push((parent, previous));
            self.set_time(parent, Some(disk::FileTime {
                modified: now,
                ..previous.unwrap_or_default()
            }));
        }
        let saved = if scratch_only && !undo.is_empty() {
            self.disk.write_back(&self.files, &undo)
        } else {
            self.disk.save(&self.files, &self.versions)
        };
        if let Err(error) = saved {
            for (path, time) in undo.into_iter().rev() {
                self.set_time(path, time);
            }
            return Err(error);
        }
        Ok(())
    }

    fn set_time(&mut self, path: &str, time: Option<disk::FileTime>) {
        match time {
            Some(time) => {
                if let Some(existing) = self.disk.times.get_mut(path) {
                    *existing = time;
                } else {
                    self.disk.times.insert(path.into(), time);
                }
            }
            None => { self.disk.times.remove(path); }
        }
    }

    // A missing entry removes the path. A present entry contains a file or a directory.
    pub(crate) fn commit(
        &mut self,
        path: String,
        entry: Option<Option<Vec<u8>>>,
        version: Option<Version>,
    ) -> Result<()> {
        self.require_writable(&path)?;
        if entry.is_some() && !self.files.contains_key(&path) && self.files.len() >= ENTRY_LIMIT {
            return Err("filesystem full");
        }
        let replace_version = entry.is_none() || version.is_some();
        let old_entry = match entry {
            Some(value) => self.files.insert(path.clone(), value.map(disk::FileData::from_vec)),
            None => self.files.remove(&path),
        };
        let old_version = if let Some(version) = version {
            self.versions.insert(path.clone(), version)
        } else if replace_version {
            self.versions.remove(&path)
        } else {
            None
        };
        if let Err(error) = self.save_changes(core::iter::once((path.as_str(), old_entry.is_some())), &disk::Times::new()) {
            match old_entry {
                Some(value) => {
                    self.files.insert(path.clone(), value);
                }
                None => {
                    self.files.remove(&path);
                }
            }
            if replace_version {
                match old_version {
                    Some(version) => {
                        self.versions.insert(path, version);
                    }
                    None => {
                        self.versions.remove(&path);
                    }
                }
            }
            return Err(error);
        }
        Ok(())
    }
}

#[derive(Clone)]
struct Services {
    agent_bridge: Rc<RefCell<Option<crate::agent::Bridge>>>,
    volume: Rc<RefCell<Volume>>,
    started: u64,
    cwd: String,
}

type AgentBridge = Rc<RefCell<Option<crate::agent::Bridge>>>;
struct BridgeCell(core::cell::UnsafeCell<Option<AgentBridge>>);
// Accessed on one CPU without yielding while cloning the slot.
unsafe impl Sync for BridgeCell {}
static MODEL_BRIDGE: BridgeCell = BridgeCell(core::cell::UnsafeCell::new(None));

fn model_bridge() -> AgentBridge {
    unsafe { (&mut *MODEL_BRIDGE.0.get()).get_or_insert_with(|| Rc::new(RefCell::new(None))).clone() }
}
fn pack_args(args: &[String]) -> String {
    let mut packed = String::new();
    for arg in args {
        packed.push_str(arg);
        packed.push('\0');
    }
    packed
}

impl Services {
    fn path(&self, name: &str) -> Result<String> {
        self.volume.borrow().resolve(&self.cwd, name)
    }

    fn store_bytes(&mut self, path: String, value: Option<Vec<u8>>) -> Result<()> {
        self.volume.borrow_mut().commit(path, Some(value), None)
    }

    fn file(&self, name: &str) -> Result<String> {
        String::from_utf8(self.bytes(name)?).map_err(|_| "binary file")
    }

    fn compile_lxe(&mut self, src: &str, dst: &str, streams: crate::utility::Streams) -> Result<()> {
        let dest = self.path(dst)?;
        if self.volume.borrow().files.get(&dest) == Some(&None) {
            return Err("is a directory");
        }
        if dest == self.path(src)? {
            return Err("output is the source file");
        }
        let source = self.file(src)?;
        let io = crate::utility::Context::new(&self.cwd, self.volume.clone(), streams);
        let (_, image) = io.enter(|| crate::user::compile(src, &source));
        self.store_bytes(dest, Some(image?))
    }

    fn with_agent<T>(
        &self,
        action: impl FnOnce(&mut crate::agent::Bridge) -> Result<T>,
    ) -> Result<T> {
        let mut bridge = self
            .agent_bridge
            .try_borrow_mut()
            .map_err(|_| "agent is busy")?;
        if bridge.is_none() {
            *bridge = Some(crate::agent::Bridge::connect()?);
        }
        action(bridge.as_mut().unwrap())
    }

    fn improve_script(&mut self, tokens: &[String]) -> Result<()> {
        const USAGE: &str = "usage: agent improve file.py \"requirement\" [--baseline old.json] [--examples new.json]";
        let [name, task, options @ ..] = tokens else {
            return Err(USAGE);
        };
        if !name.ends_with(".py") || task.trim().is_empty() || options.len() % 2 != 0 {
            return Err(USAGE);
        }
        let mut extra_baseline = None;
        let mut examples = None;
        for option in options.chunks_exact(2) {
            let [flag, file] = option else {
                return Err(USAGE);
            };
            let target = match flag.as_str() {
                "--baseline" if extra_baseline.is_none() => &mut extra_baseline,
                "--examples" if examples.is_none() => &mut examples,
                _ => return Err(USAGE),
            };
            *target = Some(agent::parse_tests(&self.file(file)?)?);
        }

        let path = self.path(name)?;
        if path == crate::agent_tools::POLICY_PATH {
            return Err("use agent tune for the agent policy");
        }
        let source = self.file(&path)?;
        let starting_version = self.volume.borrow().versions.get(&path).cloned();
        let mut tests = starting_version
            .as_ref()
            .map_or_else(Vec::new, |version| version.tests.clone());
        if let Some(extra) = extra_baseline {
            agent::merge_tests(&mut tests, extra)?;
        }
        let original = Revision { source, tests };
        let (previous, candidate) =
            self.with_agent(|bridge| agent::improve(bridge, &path, task, original, examples))?;
        let mut volume = self.volume.borrow_mut();
        if volume.files.get(&path).and_then(Option::as_deref) != Some(previous.source.as_bytes())
            || volume.versions.get(&path) != starting_version.as_ref()
        {
            return Err("active script changed during improvement");
        }
        let version = Version {
            tests: candidate.tests,
            previous: Some(previous),
        };
        volume.commit(
            path.clone(),
            Some(Some(candidate.source.into_bytes())),
            Some(version),
        )?;
        print(&format!(
            "agent: activated {path} and saved its previous version\n"
        ));
        Ok(())
    }

    fn rollback_script(&mut self, name: &str) -> Result<()> {
        let path = self.path(name)?;
        let mut volume = self.volume.borrow_mut();
        let previous = volume
            .versions
            .get(&path)
            .and_then(|version| version.previous.clone())
            .ok_or("no previous version for this script")?;
        let version = Version {
            tests: previous.tests,
            previous: None,
        };
        volume.commit(
            path.clone(),
            Some(Some(previous.source.into_bytes())),
            Some(version),
        )?;
        print(&format!("agent: restored previous version of {path}\n"));
        Ok(())
    }

    fn inspect_query(&self, tool: &str, path: &str) -> Result<String> {
        use crate::inspect;
        match (tool, path) {
            ("system", "") => Ok(inspect::system()),
            ("cpu", "") => Ok(inspect::cpu()),
            ("memory", "") => Ok(inspect::memory()),
            ("disk", "") => Ok(inspect::disk(&self.volume.borrow().disk)),
            ("filesystem", "") => Ok(inspect::filesystem(&self.volume.borrow())),
            ("list" | "stat", path) if !path.is_empty() => {
                let path = self.path(path)?;
                let volume = self.volume.borrow();
                if tool == "list" {
                    inspect::list(&volume, &path)
                } else {
                    inspect::stat(&volume, &path)
                }
            }
            _ => Err("unsupported inspection tool or invalid path argument"),
        }
    }
    fn bytes(&self, name: &str) -> Result<Vec<u8>> {
        self.volume.borrow().read_file(&self.path(name)?)
    }

    fn executable_bytes(&self, name: &str) -> Result<Vec<u8>> {
        let image = self.bytes(name)?;
        if image == BASH_ALIAS { self.bytes("/bin/bash") }
        else if image == LINKS_ALIAS { self.bytes("/bin/links") } else { Ok(image) }
    }

    // Compatibility adapters for ROM-backed applications on existing disks.
    fn legacy(&mut self, cmd: &str, args: &[String]) -> Result<u8> {
        let mut status = 0;
        self.launch_legacy(cmd, args, &mut status)?;
        Ok(status)
    }
    fn launch_legacy(&mut self, cmd: &str, tokens: &[String], status_out: &mut u8) -> Result<()> {
        let standalone = (tokens.len() == 1).then_some(cmd);
        if standalone == Some("shutdown") {
            {
                let mut volume = self.volume.borrow_mut();
                if volume.disk.scratch_dirty {
                    let Volume { disk, files, versions, .. } = &mut *volume;
                    disk.save(files, versions)?;
                }
            }
            print("Shutting down LixinOS...\n");
            // QEMU virt exposes PSCI SYSTEM_OFF via HVC at EL1.
            unsafe {
                core::arch::asm!("hvc #0", inout("x0") 0x8400_0008_u64 => _, clobber_abi("C"))
            };
            return Err("power off failed");
        }
        if standalone == Some("cecho") {
            return crate::user::run(include_bytes!("../target/user-build/echo.bin"), None);
        }
        if standalone == Some("htop") {
            return crate::foreground::run("htop", None, self.volume.clone(), self.started);
        }
        if cmd == "git" {
            let mut args = tokens[1..].to_vec();
            if args.is_empty() || args[0] == "--help" {
                print(
                    "git init | status | add PATH... | diff [--staged] | commit -m MESSAGE | log\n",
                );
                return Ok(());
            }
            let mut root = self.cwd.as_str();
            if args[0] != "init" {
                while !self
                    .volume
                    .borrow()
                    .files
                    .contains_key(&format!("{}/.git/HEAD", root.trim_end_matches('/')))
                {
                    if root == "/" {
                        return Err("not inside a Git repository");
                    }
                    root = root.rsplit_once('/').unwrap().0;
                    if root.is_empty() {
                        root = "/";
                    }
                }
            }
            if args[0] == "add" {
                if args.get(1).is_some_and(|a| a == "-A") {
                    args.remove(1);
                    if args.len() == 1 {
                        args.push(root.into());
                    }
                }
                for arg in &mut args[1..] {
                    let path = self.path(arg)?;
                    if path == root {
                        *arg = ".".into();
                        continue;
                    }
                    let relative = path
                        .strip_prefix(root.trim_end_matches('/'))
                        .and_then(|p| p.strip_prefix('/'))
                        .ok_or("path outside repository")?;
                    if relative
                        .split('/')
                        .any(|part| part.eq_ignore_ascii_case(".git"))
                    {
                        return Err("cannot stage Git metadata");
                    }
                    *arg = relative.into();
                }
            }
            return crate::git::run(self.volume.clone(), root, &args);
        }
        if cmd == "agent" {
            if let Some(mode) = tokens.get(1) {
                match mode.as_str() {
                    "chat" => {
                        let args = &tokens[2..];
                        // Keep accepting the old opt-in flag now that bypass starts on.
                        let args = if args.first().is_some_and(|arg| arg == "--auto-approve") {
                            &args[1..]
                        } else {
                            args
                        };
                        let root = match args {
                            [] => self.cwd.clone(),
                            [path] if !path.starts_with('-') => self.path(path)?,
                            _ => return Err("usage: agent chat [PROJECT]"),
                        };
                        return crate::agent_tools::chat(
                            &root, self.volume.clone(), self.agent_bridge.clone(), self.started,
                        );
                    }
                    "trace" => {
                        let root = match &tokens[2..] {
                            [] => self.cwd.clone(),
                            [path] => self.path(path)?,
                            _ => return Err("usage: agent trace [PROJECT]"),
                        };
                        return crate::agent_tools::list_traces(&root, &self.volume.borrow());
                    }
                    "run" => {
                        let [_, _, project, task] = &tokens[..] else {
                            return Err("usage: agent run PROJECT \"task\"");
                        };
                        if task.trim().is_empty() {
                            return Err("usage: agent run PROJECT \"task\"");
                        }
                        return crate::agent_tools::run(
                            &self.path(project)?,
                            task,
                            self.volume.clone(),
                            self.agent_bridge.clone(),
                        );
                    }
                    "tune" => {
                        let [_, _, task] = &tokens[..] else {
                            return Err("usage: agent tune \"task\"");
                        };
                        if task.trim().is_empty() { return Err("usage: agent tune \"task\""); }
                        return crate::agent_tools::tune(
                            task, self.volume.clone(), self.agent_bridge.clone(),
                        );
                    }
                    "system" => {
                        let [_, _, task] = &tokens[..] else {
                            return Err("usage: agent system \"task\"");
                        };
                        if task.trim().is_empty() { return Err("usage: agent system \"task\""); }
                        return crate::agent_tools::system(
                            task, self.volume.clone(), self.agent_bridge.clone(),
                        );
                    }
                    "system-rollback" => {
                        if tokens.len() != 2 { return Err("usage: agent system-rollback"); }
                        crate::agent_system::rollback(&mut self.volume.borrow_mut())?;
                        print("agent: restored the previous system files\n");
                        return Ok(());
                    }
                    "rollback-tune" => {
                        if tokens.len() != 2 { return Err("usage: agent rollback-tune"); }
                        return self.rollback_script(crate::agent_tools::POLICY_PATH);
                    }
                    "improve" => return self.improve_script(&tokens[2..]),
                    "rollback" => {
                        let [_, _, name] = &tokens[..] else {
                            return Err("usage: agent rollback file.py");
                        };
                        return self.rollback_script(name);
                    }
                    _ => (),
                }
            }
            if tokens.get(1).is_some_and(|mode| mode == "inspect") {
                let [_, _, question] = &tokens[..] else {
                    return Err("usage: agent inspect \"question\"");
                };
                if question.trim().is_empty() {
                    return Err("usage: agent inspect \"question\"");
                }
                let git_reference = String::from(agent::git_reference(&self.volume.borrow().files));
                return self.with_agent(|bridge| {
                    crate::agent::inspect(bridge, question, &self.cwd, &git_reference, |tool, path| {
                        self.inspect_query(tool, path)
                    })
                });
            }
            let [_, name, task] = &tokens[..] else {
                return Err(
                    "usage: agent run, agent system, agent system-rollback, agent tune, agent rollback-tune, agent inspect, agent improve, agent rollback, or agent output.py \"task\"",
                );
            };
            if task.trim().is_empty() || !name.ends_with(".py") {
                return Err("usage: agent output.py \"task\"");
            }
            let path = self.path(name)?;
            if path == crate::agent_tools::POLICY_PATH {
                return Err("use agent tune for the agent policy");
            }
            if self.volume.borrow().files.contains_key(&path) {
                return Err("agent output already exists");
            }
            let revision = self.with_agent(|bridge| agent::generate(bridge, &path, task))?;
            let mut volume = self.volume.borrow_mut();
            if volume.files.contains_key(&path) {
                return Err("agent output already exists");
            }
            let version = Version {
                tests: revision.tests,
                previous: None,
            };
            volume.commit(
                path.clone(),
                Some(Some(revision.source.into_bytes())),
                Some(version),
            )?;
            print(&format!("agent: saved {path}\n"));
            return Ok(());
        }
        if cmd == "paint" {
            #[cfg(feature="retina")]
            if crate::graphics::popup_active() { return Err("paint needs the main console"); }
            #[cfg(not(feature = "retina"))]
            return Err("paint needs the graphical --window or --split launcher");
            #[cfg(feature = "retina")]
            {
                let name = match &tokens[..] {
                    [_] => "drawing.svg",
                    [_, name] => name.as_str(),
                    _ => return Err("usage: paint [drawing.svg]"),
                };
                return crate::foreground::run(
                    cmd,
                    Some(self.path(name)?),
                    self.volume.clone(),
                    self.started,
                );
            }
        }
        if matches!(cmd, "nano" | "kilo") {
            let [cmd, name] = &tokens[..] else {
                return Err("expected editor filename");
            };
            return crate::foreground::run(
                cmd,
                Some(self.path(name)?),
                self.volume.clone(),
                self.started,
            );
        }
        if cmd == "tcc" {
            match tokens {
                [_, flag, path, extra @ ..] if flag == "-run" => {
                    let mut source = self.file(path)?;
                    source.push('\0');
                    for token in extra {
                        let arg = token;
                        source.push_str(arg);
                        source.push('\0');
                    }
                    let (_, status) = crate::utility::with_io(
                        &self.cwd,
                        self.volume.clone(),
                        None,
                        false,
                        || crate::user::launch(crate::user::TCC, Some((path, &source)), false),
                    )?;

                    print(&format!("[exit {status}]\n"));
                    *status_out = status;
                    return Ok(());
                }
                [_, src, flag, dst] if flag == "-o" => {
                    return self.compile_lxe(src, dst, crate::utility::Streams::default());
                }
                _ => return Err("usage: tcc -run source.c | tcc source.c -o output"),
            }
        }
        Err("unknown operation")
    }
}

fn old_stock_ls_source(sig: (usize, u32)) -> bool {
    sig == (2896, 0xceb3_be74) || matches!(sig,
        (2533, 0xae82_4921) | (2790, 0x5ee0_c525) | (3552, 0x645f_1537))
}

fn old_stock_agent_source(sig: (usize, u32)) -> bool {
    matches!(sig,
        (1482, 0xb54d_2c61) | (1538, 0x8270_ae19) | (1749, 0xa1b4_15cf)
        | (1821, 0x3298_95ab) | (7457, 0x06a8_d956))
}

fn install_sources(volume: &mut Volume) -> Result<()> {
    const SOURCES: &[(&str, Option<&[u8]>)] =
        include!(concat!(env!("OUT_DIR"), "/user-sources.rs"));
    let mut changed = Vec::new();
    let result = (|| {
        for &(path, contents) in SOURCES {
            if let Some(existing) = volume.files.get(path) {
                if existing.is_none() != contents.is_none() {
                    return Err("source path conflicts with an existing entry");
                }
                let upgrade = existing.as_ref().is_some_and(|file| {
                    let sig = file.sig();
                    match path {
                    "/lib/git.elf" => sig == (672928, 0x26d8dade),
                    "/src/git/platform.c" => sig == (10427, 0x80d9f358),
                    "/src/kernel/files.c" => matches!(sig, (6340, 0xb39ddf09) | (7226, 0xb74cbafd)),
                    "/src/kernel/el1.S" => matches!(sig,
                        (7195, 0xde9cf0cf) | (7415, 0x8331d63e) | (6781, 0x1550_5ac6) | (6935, 0xfea1_f971) | (6152, 0x04169dda)),
                    "/src/kernel/memory.c" => sig == (2463, 0x2f57463c),
                    "/src/kernel/build-fs.h" => sig == (10631, 0x71861716),
                    "/src/kernel/agent-service.h" => sig == (5350, 0x73816c07),
                    "/src/kernel/mux.c" => sig == (3202, 0xb9614109),
                    "/src/kernel/stream.c" => matches!(sig, (1788, 0xb54adfb7) | (1824, 0xf1cef9ef) | (6569, 0x9d1c6f2c)),
                    "/src/gui/text-clipboard.h" => matches!(sig, (554, 0x4390c1c2) | (574, 0xc60846e9)),
                    "/src/gui/lixin-gui.h" => matches!(sig, (5983, 0x48ffa263) | (6955, 0x7ea945b1)),
                    "/src/gui/han-font.c" => matches!(sig, (3460, 0xed2a1b7c)),
                    "/src/gui/file-dialog.h" => matches!(sig, (8026, 0x7619e968) | (8128, 0x740cf8d3) | (8141, 0x932637e3)),
                    "/src/gui/file-path.h" => matches!(sig, (881, 0x9c550f55)),
                    "/src/desktop/window-widgets.h" => matches!(sig, (4056, 0xd801c4d5)),
                    "/src/desktop/widgets-demo.c" => matches!(sig, (1453, 0x3a71bd6e)),
                    "/src/gui/widgets.h" => matches!(sig, (4050, 0xf3f3a912)),
                    "/src/gui/text-editor.c" => matches!(sig, (12771, 0xac06779f) | (7684, 0xe1836cd7) | (9342, 0xa0428e2a)),
                    "/src/desktop/settings.c" => matches!(sig, (14296, 0xa905ee68) | (24677, 0xfaf7a280) | (25675, 0x2ec6825d) | (25749, 0xd2ec18f2)),
                    "/src/desktop/window-app.h" => matches!(sig, (3648, 0x8f30269f) | (3705, 0x9989efc7) | (4348, 0x8876d233)),
                    "/src/gui/mouse-cursor.h" => matches!(sig, (1327, 0x5072f402)),
                    "/src/kernel/input.c" => matches!(sig, (5834, 0x48dcd6bf) | (6623, 0x0ee8cd99) | (6914, 0xe9073641)),
                    "/src/kernel/loader-sources" => sig == (80, 0x7534928c),
                    "/src/kernel/smp.c" => sig == (3665, 0xb4caae7f),
                    "/src/kernel/smp.h" => sig == (443, 0xf568791d),
                    "/src/kernel/smp-arch.S" => sig == (5341, 0x9040ebf0),
                    "/src/kernel/sources" => matches!(sig, (1080, 0x2cdd_da4a) | (1100, 0x62652e92) | (1128, 0xdf1a973c)),
                    "/src/kernel/process.c" => matches!(sig, (76827, 0x2a29b0ed) |
                        (66757, 0x0f788aa2) | (70784, 0x50475a8f) | (72567, 0x587e087f) | (75563, 0xb1234c0e) | (63989, 0x6d114b70) | (64480, 0x3ff4e081) | (64518, 0x34819eec) | (65851, 0x67f4783b) | (66460, 0x267f4e60) | (67371, 0x3aa2b0e0) | (13400, 0x0898_6dcc) | (45380, 0x5748_cd25) | (45801, 0x0a75_b6e9) | (52797, 0x254e_c10a) | (58020, 0x29ef_4f7f) | (58028, 0x44859e47) | (70123, 0x07cd_b98b) | (76698, 0xbf32fdc7)),
                    "/src/kernel/net.c" => sig == (8078, 0xfdb2_d249),
                    "/src/kernel/task.c" => matches!(sig, (10673, 0x8b008d68) | (5822, 0x31fc4674) | (9407, 0x59d0dc5b) | (5587, 0xb041_d7c8) | (5698, 0x6ff2_a8b3) | (8251, 0x183c59e7)),
                    "/src/kernel/disk.c" => matches!(sig,
                        (8700, 0xa8ba_c0e5) | (20901, 0xf5e0_c618) | (20999, 0x7f64_447e) | (27954, 0x7c21fc3b)),
                    "/src/kernel/native.h" => matches!(sig,
                        (5232, 0x65cbabc4) | (6161, 0xd5328656) | (6781, 0x88b0ef54) | (999, 0x055d_fc37) | (4091, 0xf3f4_5034) | (4367, 0x5e65_fa6c) | (7907, 0x0353213c)),
                    "/src/kernel/display.c" => matches!(sig,
                        (35720, 0x1ed640d3) | (4726, 0x739a_83ce) | (25975, 0xc6a3_fe7b) | (27711, 0x4571_61b1) | (35309, 0xb11c_5e77)),
                    "/src/kernel/proc.c" => matches!(sig, (3080, 0x5026_f57d) | (3135, 0xee1ae832) | (3094, 0x934c_4a47) | (3085, 0xcd825883)),
                    "/src/kernel/kernel.c" => matches!(sig,
                        (2937, 0xe0c9f33e) | (1090, 0x4f0a_818c) | (2768, 0x69ee_92b2) | (2768, 0x7556_94ab) | (3719, 0x1df08358)),
                    "/src/console.h" => matches!(sig, (569, 0x5646a81d)),
                    "/src/build/rebuild.c" => matches!(sig, (2668, 0xc7f2_1369) | (2761, 0x22fe_f9c0) | (2951, 0x9183_7f72) | (3094, 0x49c8_a199)),
                    "/src/core/text.h" => matches!(sig,
                        (12257, 0xf270_f24b) | (13019, 0x5b47_073d) | (13362, 0x31d5_48d4)),
                    "/src/core/commands.c" => matches!(sig,
                        (3511, 0x01eb_f07b) | (3760, 0x17da_1a6e) | (4597, 0x9ae8_b92f)
                        | (5354, 0xc3cd_3d9e) | (8010, 0xed04_3ba5) | (10663, 0xa550_c08e)),
                    "/src/tinycc/port.c" => matches!(sig, (8781, 0x182ee5f9) | (9991, 0xad0fa92c)),
                    "/src/tcc/port.c" => matches!(sig, (8781, 0x182ee5f9) | (9991, 0xad0fa92c)),
                    "/src/tinycc/slots.def" => matches!(sig, (2584, 0xc7c619ee)),
                    "/src/bash/bash.c" => matches!(sig,(28822, 0x931e3765) | (32111, 0x6ce062f4) | (21087, 0x5a27_2604)|(21217,0xd799627c)|(23905,0xf0d95640)|(24657,0x960d3663)|(28403,0x43e717dd)|(28478,0x1f9c15f8)|(28486,0x775d056a)),
                    "/src/ls/ls.c" => old_stock_ls_source(sig),
                    "/src/core/discovery.h" => sig == (5547, 0x2cff_7a65),
                    "/src/links/config.h" => sig == (1058, 0xeca2a4c8),
                    "/src/links/load.h" => sig == (293, 0xec3d052c),
                    "/src/links/platform.c" => sig == (1612, 0xce9afe83),
                    "/src/links/prepare.py" => sig == (2504, 0x1c44bb5a),
                    "/src/links/window.c" => matches!(sig, (2372, 0x3f7daa1f) | (3315, 0x9b41_4dd9)),
                    "/src/links/window.h" => sig == (510, 0x8da8_5f29),
                    "/src/desktop/Makefile" => matches!(sig, (670, 0x1854_d8c8) | (644, 0x3ce7_0dd2)),
                    "/src/gui/calculator.c" => sig == (4723, 0xebc3_db1a),
                    "/src/links/Makefile" => matches!(sig, (1208, 0xc0a53c5c) | (1658, 0x37e74e62) | (2157, 0x37179398) | (2422, 0x5721d2f4) | (2481, 0xd410c763) | (2557, 0x58f91ab4) | (2572, 0x9ad654f5) | (2589, 0xe1f55ea7) | (2608, 0xa1785272) | (2655, 0xc0a6fa52)),
                    "/src/links/load.c" => matches!(sig, (6339, 0x199e4863) | (7009,0x36785356)),
                    "/src/links/port.c" => matches!(sig, (8914, 0x97f620b0) | (12503,0x0abe84af) | (17771, 0xe2c7_3fd0)),
                    "/src/links/COPYING" => matches!(sig, (18719, 0x0f0b0cd4) | (19830,0x8eaa86ec)),
                    "/src/ssh/Makefile" => matches!(sig, (1131, 0xf2ed6e21) | (1234, 0xe7c4f654)),
                    "/src/ssh/src/sftp.rs" => sig == (18734, 0x87ba63dd),
                    "/src/ssh/port.c" => matches!(sig, (1810, 0xc6d26543) | (2380, 0xba9d8ff5)),
                    "/src/ssh/src/lib.rs" => matches!(sig, (11370, 0x81c5b726) | (15613, 0x276b3d15)),
                    "/src/desktop/desktop.c" => matches!(sig, (13586, 0xb482580b) | (15502, 0xa0d13781) | (17767, 0x7fc65414) | (27008, 0x710e8655) | (31136, 0x0c2a4e1d) | (38720, 0x5490a6f8) | (41064, 0x15d7bcb7) | (42006, 0xff99e8d9) | (42192, 0x64ea1e04) | (58688, 0x027546d7) | (64603, 0xf68b6587) | (65590, 0x596da4aa) | (65793, 0x08f47430) | (66112, 0x7e896c25) | (66202, 0x7df78228) | (69118, 0xb2cf597a) | (69129, 0x27c89e82) | (70754, 0x821c6822) | (71535, 0x8b43d1c3) | (71535, 0xac529004) | (9781, 0x5daec732)),
                    "/src/desktop/window-client.h" => matches!(sig, (442, 0x1881d856) | (527, 0xe99b_c843) | (896, 0xb7febea3) | (1299, 0x63c0ceb3) | (1358, 0x88bcb7d0)),
                    "/src/desktop/window-client-host.h" => matches!(sig, (7009, 0xc90249d1) | (2979, 0x8082_8916) | (3666, 0xb572_7954) | (7416, 0x92fe5a8b) | (8411, 0x2a0c9169) | (10292, 0xfdea4f71)),
                    "/src/desktop/README.md" => matches!(sig, (10213, 0x2943ae71) | (10635, 0xd3ab206d) | (11599, 0xd16763b0) | (11873, 0x4faf09cd) | (12196, 0x97b20e61) | (13031, 0x0647302c) | (1615, 0x0c62067e) | (3469, 0x1c0f3687) | (6363, 0xda6d6a91) | (7025, 0xdeb37807) | (962, 0x923c2f17) | (13458, 0xddd2_c35b)),
                    "/src/desktop/font.swift" => sig == (1126,0xda7679a1),
                    "/src/foreground/nano.rs" => matches!(sig,
                        (8519, 0xc23c_73b8) | (15861, 0xea0a_62d3)),
                    "/src/agent/run.c" => old_stock_agent_source(sig),
                    "/src/agent/native.py" => matches!(sig,
                        (43529, 0x8602_3976) | (44798, 0x74a6_3448) | (51749, 0x4b7e_44aa) | (52593, 0xe29d_185d)
                        | (55005, 0xe4a1_d5c6) | (56717, 0xf1b5_d303) | (57305, 0x30c4_b446) | (57774, 0xd085_d9bb)
                        | (58718, 0xeedd_835f)),
                    "/src/agent/desktop.py" => matches!(sig,
                        (16576, 0x4c1d_cde8) | (23595, 0x60ca4ec3) | (24673, 0xc951_f5ac) | (28949, 0xf838_f220) | (29500, 0xa0f7_db02)),
                    "/src/agent/desktop.md" => matches!(sig,
                        (2159, 0x9483_879f) | (2615, 0x7efc_5c65) | (2913, 0x7343_1a7f) | (3095, 0xfcdb_075f)),
                    "/src/agent/README.md" => matches!(sig,
                        (1095, 0x0ddc_f2cf) | (1620, 0x2823_d007) | (1972, 0xc2ed_6bdd) | (2114, 0x1dcb_e514)
                        | (2180, 0xe59b_f15f) | (3087, 0x09b0_6b09)),
                    "/src/agent/chat.md" => matches!(sig,
                        (1897, 0xaa32_1408) | (2681, 0x3bd1_dea5) | (3164, 0x868a_4295)
                        | (3268, 0x8140_54a4) | (3382, 0x97a7_59b4) | (4013, 0xc84e_5efc)
                        | (4411, 0x303a_d61a) | (5392, 0xb6f8_95d9)),
                    "/src/python/Makefile" => sig == (1043, 0xde44_90f8),
                    "/src/python/mpconfigport.h" | "/src/micropython/mpconfigport.h" =>
                        matches!(sig, (912, 0x3067_fe3d) | (1189, 0xef7d_fa2c)),
                    "/src/python/mphalport.h" | "/src/micropython/mphalport.h" =>
                        sig == (242, 0x1aa8_0e28),
                    "/src/python/port.c" | "/src/micropython/port.c" =>
                        matches!(sig, (1975, 0x9f70_3bdc) | (2834, 0x7532_1644)),
                    "/src/python/system.c" | "/src/micropython/system.c" =>
                        sig == (10903, 0xeb2e_454e),
                    "/src/micropython/sources.txt" => sig == (4214, 0x2357_d32d),
                    "/src/micropython/genhdr/moduledefs.h" => sig == (622, 0x7c78_205f),
                    "/src/micropython/genhdr/qstrdefs.generated.h" => sig == (9548, 0xaf3a_e6e4),
                    "/src/micropython/genhdr/root_pointers.h" => sig == (86, 0x8c0f_836f),
                    "/src/tcc/lixin.h" => sig == (1775, 0xf41f_67b3) || sig==(2078,0x2c0d57c9),
                    "/src/tcc/api.c" => sig==(3326,0x77cffc86),
                    "/src/tcc/slots.def" => matches!(sig, (2584, 0xc7c619ee)) || sig==(1371,0x61f63c00),
                    "/src/sh/main.rs" => matches!(sig, (12860, 0xce52_7fea) | (16896, 0xee4c_2cce)),
                    "/src/sh/execute.rs" => matches!(sig,
                        (6893, 0xf0c3_d2b5) | (6898, 0xfbba_c8c9)),
                    "/src/sh/abi.rs" => sig == (530, 0x1613_cdd9) || sig==(601,0x623df15f),
                    _ => false,
                }});
                if upgrade && existing.as_deref() != contents {
                    let old = volume.files.insert(path.into(), contents.map(disk::FileData::from));
                    changed.push((path, old));
                }
                continue;
            }
            if volume.files.len() >= ENTRY_LIMIT {
                return Err("filesystem full");
            }
            // Keep edits made on older disks, including the original root copies.
            let legacy = match path {
                "/src/ls/ls.c" => volume.files.get("/ls.c").filter(|entry| {
                    !entry.as_ref().is_some_and(|f| old_stock_ls_source(f.sig()))
                }),
                "/src/agent/run.c" => volume.files.get("/agent.c").filter(|entry| {
                    !entry.as_ref().is_some_and(|f| old_stock_agent_source(f.sig()))
                }),
                _ => None,
            };
            let data = legacy.and_then(Option::as_deref).or(contents);
            let old = volume.files.insert(path.into(), data.map(disk::FileData::from));
            changed.push((path, old));
        }
        if !changed.is_empty() {
            volume.save_changes(changed.iter().map(|(path, old)| (*path, old.is_some())), &disk::Times::new())?;
        }
        Ok(())
    })();
    if result.is_err() {
        for (path, old) in changed {
            if let Some(old) = old {
                volume.files.insert(path.into(), old);
            } else {
                volume.files.remove(path);
            }
        }
    }
    result
}

pub fn run() -> ! {
    let started = crate::monitor::ticks();
    let (mut disk, files, versions) = crate::disk::Disk::mount().unwrap_or_else(|err| {
        print("Mount failed: ");
        print(err);
        print("\n");
        crate::idle()
    });
    let history = disk.load_history().unwrap_or_else(|err| {
        print(err);
        print("\n");
        VecDeque::new()
    });
    let mut volume = Volume {
        disk,
        files,
        versions,
        agent_trace_writes: 0,
    };
    let mut boot_changed: Vec<(String, Option<Option<disk::FileData>>)> = Vec::new();
    let stage = |volume: &mut Volume,
                 changed: &mut Vec<(String, Option<Option<disk::FileData>>)>,
                 path: String,
                 entry: Option<Vec<u8>>|
     -> Result<()> {
        if !volume.files.contains_key(&path) && volume.files.len() >= ENTRY_LIMIT {
            return Err("filesystem full");
        }
        let old = volume.files.insert(path.clone(), entry.map(disk::FileData::from_vec));
        changed.push((path, old));
        Ok(())
    };
    match volume.files.get("/bin") {
        None => {
            if let Err(err) = stage(&mut volume, &mut boot_changed, "/bin".into(), None) {
                print(&format!("Could not create /bin: {err}\n"));
            }
        }
        Some(Some(_)) => print("Existing /bin is a file; choose another PATH directory.\n"),
        Some(None) => (),
    }
    if volume.files.get("/bin") == Some(&None) {
        let old_bash = volume.files.get("/bin/bash").and_then(Option::as_ref)
            .is_some_and(|b| matches!(b.sig(), (23720, 0xb2ca_0c34) | (23776, 0x77f085b2) | (26928, 0x8968_5afc) | (27976, 0x7c17_a489) | (31528, 0xd918_f074) | (31600, 0x89a2_a041) | (31608, 0x9d07_19bc) | (31800, 0xc1129718) | (35696, 0x5d5f6f59)));
        if !volume.files.contains_key("/bin/bash") || old_bash {
            // Bootstrap the C shell with the same guest compiler users invoke.
            let built = crate::user::compile("/src/bash/bash.c", include_str!("../../User/bash/bash.c"))
                .and_then(|image| stage(&mut volume, &mut boot_changed, "/bin/bash".into(), Some(image)));
            if let Err(error) = built { print(&format!("Could not install Bash: {error}\n")); }
        }
        const SH_ELF: &[u8] = include_bytes!("../target/user-build/sh.elf");
        let old_shell = volume.files.get("/bin/sh").and_then(Option::as_ref)
            .is_some_and(|b| matches!(b.sig(),
                (72656, 0xc93c_11f5 | 0x7df0_c3cb) | (93136, 0x65cc_454b))
                || (b.len() == SH_ELF.len() && b.as_slice() == SH_ELF));
        if !volume.files.contains_key("/bin/sh") || old_shell {
            let login: &[u8] = if volume.files.get("/bin/bash").and_then(Option::as_deref).is_some_and(runnable) {
                BASH_ALIAS
            } else {
                SH_ELF
            };
            if let Err(err) = stage(&mut volume, &mut boot_changed, "/bin/sh".into(), Some(login.to_vec())) {
                print(&format!("Could not install /bin/sh: {err}\n"));
            }
        }
        let agent_path = crate::agent_tools::PROGRAM_PATH;
        let old_agent = volume.files.get(agent_path).and_then(Option::as_ref)
            .is_some_and(|bytes| matches!(bytes.sig(),
                (5320, 0x8df6_54f6) | (5568, 0xfb20_bfa9) | (5760, 0x79f4_37fc)
                | (5824, 0xc33f_35b8) | (9264, 0x91c1_0b0c)));
        if !volume.files.contains_key(agent_path) || old_agent {
            if let Err(error) = stage(
                &mut volume,
                &mut boot_changed,
                agent_path.into(),
                Some(crate::agent_tools::PROGRAM.to_vec()),
            ) {
                print(&format!("Could not install {agent_path}: {error}\n"));
            }
        }
        // Compile these commands in the guest as LXE1. Replace only old stock launchers.
        for name in ["cp", "mv", "rm", "mkdir", "rmdir", "touch"] {
            let path = format!("/bin/{name}");
            let legacy_len = LAUNCHER_PREFIX.len() + name.len();
            let needs = match volume.files.get(&path) {
                None => true,
                Some(Some(file)) if file.len() == legacy_len => {
                    let legacy = [LAUNCHER_PREFIX, name.as_bytes()].concat();
                    file.as_slice() == legacy.as_slice()
                }
                _ => false,
            };
            if needs {
                let source = format!("#define APP {name}\n{}", include_str!("../../User/core/files.c"));
                let result = crate::user::compile("/src/core/files.c", &source)
                    .and_then(|image| stage(&mut volume, &mut boot_changed, path.clone(), Some(image)));
                if let Err(error) = result { print(&format!("Could not install {path}: {error}\n")); }
            }
        }
        if !volume.files.contains_key("/bin/xargs") {
            let source = include_str!("../../User/core/xargs.c");
            if let Err(error) = crate::user::compile("/src/core/xargs.c", source)
                .and_then(|image| stage(&mut volume, &mut boot_changed, "/bin/xargs".into(), Some(image)))
            {
                print(&format!("Could not install /bin/xargs: {error}\n"));
            }
        }
        for name in COMMANDS {
            let path = format!("/bin/{name}");
            let old = volume.files.get(&path);
            let native = crate::utility::installed(name);
            let legacy_len = LAUNCHER_PREFIX.len() + name.len();
            let is_legacy = native.is_some()
                && old.and_then(Option::as_ref).is_some_and(|f| {
                    f.len() == legacy_len && {
                        let legacy = [LAUNCHER_PREFIX, name.as_bytes()].concat();
                        f.as_slice() == legacy.as_slice()
                    }
                });
            let sig = old.and_then(Option::as_ref).map(disk::FileData::sig);
            let old_core = sig.is_some_and(|s| old_core_elf(name, s));
            // Update the previous stock ELF while keeping guest-built replacements.
            let old_ls = *name == "ls" && sig.is_some_and(|s| {
                matches!(s,
                    (5896, 0x609e_7e97) | (6024, 0x18e2_b7e2) | (6056, 0x6724_d0b1)
                    | (7384, 0x926a_196f) | (8136, 0xd7b7_8e12))
            });
            let old_lust = *name == "lust" && sig.is_some_and(|s| {
                matches!(s,
                    (598112, 0x1a85_50b4) | (600320, 0xab2b_9bef) | (601104, 0x4b14_9bb2) | (606016, 0x0fdb_cc69) | (610544, 0x690c_2452) | (617888, 0x0846_68b7))
            });
            if old.is_none()
                || is_legacy
                || (*name == "sftp" && sig == Some((167368, 0x35558d40)))
                || (*name == "ssh" && sig.is_some_and(|s|
                    matches!(s, (134744, 0xe7907f77) | (142400, 0x8dfb7311) | (167368, 0xd5198a99))))
                || (*name == "links" && sig.is_some_and(|s|
                    matches!(s, (316056, 0x03004fc3) | (532008, 0x2514945e) | (801016, 0xe7615558) | (841528, 0xf20e_d920))))
                || (*name == "stat" && sig == Some((20328, 0x61e3_c36a)))
                || old_core || old_ls || old_lust || (*name == "desktop" && sig.is_some_and(|s|
                    matches!(s, (55296, 0x9c478e70) | (59752, 0xb2dd3ede) | (60360, 0x5fc9_765f) | (60400, 0x50bc28a4) | (61176, 0x1ddd_ee18) | (61120, 0xcf10_d7c0) | (852104, 0x7791_59a7) | (854120, 0xd486_8cbd) | (857160, 0x35b9_cd0f) | (859240, 0xa424_015a) | (868568, 0x4b92cc1c) | (871976, 0x2f2d7c77) | (873464, 0x1eb38c5b) | (874184, 0x96316ca2) | (875336, 0x1b61ca3e) | (875400, 0x5932489f) | (880008, 0x6cb7_29ce) | (880456, 0x01ce6f9b) | (880456, 0x1944a089))))
            {
                let launcher = if *name == "links2" {
                    LINKS_ALIAS.to_vec()
                } else {
                    native.map_or_else(|| [LAUNCHER_PREFIX, name.as_bytes()].concat(), <[u8]>::to_vec)
                };
                if let Err(error) = stage(&mut volume, &mut boot_changed, path, Some(launcher)) {
                    print(&format!("Could not install /bin/{name}: {error}\n"));
                    break;
                }
            }
        }
    }
    if !boot_changed.is_empty() {
        if let Err(error) = volume.save_changes(
            boot_changed.iter().map(|(path, old)| (path.as_str(), old.is_some())),
            &disk::Times::new(),
        ) {
            for (path, old) in boot_changed.into_iter().rev() {
                if let Some(old) = old {
                    volume.files.insert(path, old);
                } else {
                    volume.files.remove(&path);
                }
            }
            print(&format!("Could not save /bin updates: {error}\n"));
        }
    }
    if let Err(error) = install_sources(&mut volume) {
        print(&format!("Could not install /src: {error}\n"));
    }
    if let Err(error) = crate::boot_package::install_cpython(&mut volume) {
        print(&format!("Could not install CPython: {error}\n"));
    }
    let services = Services {
        volume: Rc::new(RefCell::new(volume)),
        agent_bridge: model_bridge(),
        started,
        cwd: "/".into(),
    };
    crate::scheduler::init();
    let install_editor = services.volume.borrow().files.get("/bin/text-editor").is_none_or(|entry|
        entry.as_ref().is_some_and(|file| matches!(file.sig(), (276392, 0x1eb439d2) | (276496, 0x085f21f5) | (278400, 0xdb0261bf))));
    if install_editor {
        let io = crate::utility::Context::new("/", services.volume.clone(), crate::utility::Streams::default());
        let (_, built) = io.enter(|| crate::user::compile("/src/gui/text-editor.c",
            include_str!("../../User/gui/text-editor.c")));
        if let Err(error) = built.and_then(|image| services.volume.borrow_mut()
            .commit("/bin/text-editor".into(), Some(Some(image)), None)) {
            print(&format!("Could not install graphical text editor: {error}\n"));
        }
    }
    #[cfg(feature = "split")]
    {
        let other = services.clone();
        let initial = history.clone();
        crate::scheduler::spawn(alloc::boxed::Box::new(move || Ok(session(other, initial))),1).unwrap();
    }
    let status=session(services,history);
    crate::scheduler::finish(Ok(status))
}

fn session(services: Services, history: VecDeque<String>) -> u8 {
    let image = services.executable_bytes("/bin/sh").ok();
    let result = match image {
        Some(image) if crate::user::process_image(&image).is_some() || crate::user::is_executable(&image) => {
            io::run(services, history, image, cfg!(feature = "split"))
        }
        Some(image) => {
            // A locally compiled replacement can also serve as the login program.
            crate::utility::with_io("/", services.volume.clone(), None, false, || {
                if crate::utility::load_segments(&image).is_some() {
                    crate::utility::launch_program(&image, "/", "")
                        .and_then(|status| u8::try_from(status).map_err(|_| "invalid exit status"))
                } else {
                    Err("invalid login program")
                }
            })
            .map(|(_, status)| status)
        }
        None => Err("missing /bin/sh"),
    };
    if let Err(e) = result {
        print("init: ");
        print(e);
        print("\n");
    }
    result.unwrap_or(1)
}
