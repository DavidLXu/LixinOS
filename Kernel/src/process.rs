#[path = "process_io.rs"]
mod io;

use crate::{
    agent::{self, Revision, Version, Versions},
    print,
};
use alloc::{collections::VecDeque, format, string::String, vec::Vec};
pub(crate) use io::{active, agent_command, agent_noninteractive, capture_agent_output, exchange, python_system, spawn_program};
#[cfg(feature = "split")]
pub(crate) use io::ready_on_read;

use alloc::rc::Rc;
use core::cell::RefCell;

type Result<T> = core::result::Result<T, &'static str>;
use crate::disk::{self, ENTRY_LIMIT, FILE_LIMIT, HISTORY_LIMIT};
const COMMANDS: &[&str] = &[
    "agent", "cat", "cd", "cecho", "clear", "desktop", "echo", "git", "grep", "help", "htop", "kilo", "links", "links2", "ls",
    "head", "tail", "tee", "true", "false", "sleep", "date", "sort", "uniq", "find", "ps", "df", "stat", "tree", "du", "cmp", "diff", "nl", "cut", "tr", "sed", "seq", "basename", "dirname", "test", "printf",
    "lust", "nano", "netinfo", "cpuinfo", "paint", "pwd", "python", "ssh", "shutdown", "sysinfo", "tcc", "uname", "wc",
];

// A system launcher names a handler supplied by the running kernel.
const LAUNCHER_PREFIX: &[u8] = b"LXcmd1\n";

fn system_handler(bytes: &[u8]) -> Option<&'static str> {
    let name = bytes.strip_prefix(LAUNCHER_PREFIX)?;
    COMMANDS.iter().copied().find(|cmd| cmd.as_bytes() == name)
}

fn old_core_elf(name: &str, bytes: &[u8]) -> bool {
    if matches!((name, bytes.len()),
        ("grep" | "find", 12424 | 12648))
    {
        return matches!((name, disk::checksum(bytes)),
            ("grep", 0xc9cf_ac87 | 0xa1f8_1447) | ("find", 0x9d0d_3de5 | 0x5df1_2023));
    }
    let (size, sums) = match name {
        "cat" => (5872, [0xbece_70c8, 0xb4d9_aa74]),
        "grep" => (5872, [0x609d_e03c, 0x2c01_ba6b]),
        "wc" => (5880, [0x9647_b6a6, 0x264c_8e3e]),
        _ => return false,
    };
    (bytes.len() == size || bytes.len() == size + 464) && sums.contains(&disk::checksum(bytes))
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
}

pub(crate) enum Entry<'a> {
    Directory,
    File(&'a [u8]),
    ProcFile(crate::procfs::File),
}

impl Entry<'_> {
    pub(crate) fn size(&self) -> Option<usize> {
        match self {
            Self::Directory => None,
            Self::File(bytes) => Some(bytes.len()),
            Self::ProcFile(_) => Some(0),
        }
    }
}

impl Volume {
    fn proc_mounted(&self) -> bool {
        !self.files.contains_key(crate::procfs::ROOT)
    }

    pub(crate) fn is_virtual_proc(&self, path: &str) -> bool {
        crate::procfs::contains(path) && self.proc_mounted()
    }

    pub(crate) fn require_writable(&self, path: &str) -> Result<()> {
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
        self.files.get(path).map(|data| data.as_deref().map_or(Entry::Directory, Entry::File))
    }

    pub(crate) fn read_file(&self, path: &str) -> Result<Vec<u8>> {
        match self.entry(path).ok_or("file not found")? {
            Entry::Directory => Err("is a directory"),
            Entry::File(bytes) => Ok(bytes.to_vec()),
            Entry::ProcFile(file) => Ok(file.read().into_bytes()),
        }
    }

    pub(crate) fn children(&self, path: &str, prefix: &str, after: Option<&str>)
        -> impl Iterator<Item = (&str, Entry<'_>)> {
        let matches = move |name: &str| name.starts_with(prefix) && after.is_none_or(|key| key < name);
        let mut proc_root = (path == "/" && self.proc_mounted() && matches("proc"))
            .then_some(("proc", Entry::Directory));
        let mut disk = disk::children_matching(&self.files, path, prefix, after)
            .map(|(name, data)| (name, data.as_deref().map_or(Entry::Directory, Entry::File)))
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
        for (path, existed) in changes {
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
        if let Err(error) = self.disk.save(&self.files, &self.versions) {
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
            Some(value) => self.files.insert(path.clone(), value),
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
            ("filesystem", "") => Ok(inspect::filesystem(&self.volume.borrow().files)),
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
                    let dest = self.path(dst)?;
                    if self.volume.borrow().files.get(&dest) == Some(&None) {
                        return Err("is a directory");
                    }
                    if dest == self.path(src)? {
                        return Err("output is the source file");
                    }
                    let source = self.file(src)?;
                    let streams = crate::utility::Streams::new(None, false);
                    let io = crate::utility::Context::new(&self.cwd, self.volume.clone(), streams);
                    let (_, image) = io.enter(|| crate::user::compile(src, &source));
                    let image = image?;
                    return self.store_bytes(dest, Some(image));
                }
                _ => return Err("usage: tcc -run source.c | tcc source.c -o output"),
            }
        }
        Err("unknown operation")
    }
}

fn old_stock_ls_source(bytes: &[u8]) -> bool {
    (bytes.len(), disk::checksum(bytes)) == (2896, 0xceb3_be74) || matches!((bytes.len(), disk::checksum(bytes)),
        (2533, 0xae82_4921) | (2790, 0x5ee0_c525) | (3552, 0x645f_1537))
}

fn old_stock_agent_source(bytes: &[u8]) -> bool {
    matches!((bytes.len(), disk::checksum(bytes)),
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
                let upgrade = existing.as_deref().is_some_and(|bytes| match path {
                    "/src/kernel/el1.S" => matches!((bytes.len(), disk::checksum(bytes)),
                        (6781, 0x1550_5ac6) | (6935, 0xfea1_f971)),
                    "/src/kernel/input.c" => (bytes.len(), disk::checksum(bytes)) == (5834, 0x48dc_d6bf),
                    "/src/kernel/loader-sources" => (bytes.len(), disk::checksum(bytes)) == (80, 0x7534928c),
                    "/src/kernel/sources" => (bytes.len(), disk::checksum(bytes)) == (1080, 0x2cdd_da4a),
                    "/src/kernel/process.c" => matches!((bytes.len(), disk::checksum(bytes)),
                        (13400, 0x0898_6dcc) | (45380, 0x5748_cd25) | (45801, 0x0a75_b6e9) | (52797, 0x254e_c10a) | (58020, 0x29ef_4f7f)),
                    "/src/kernel/net.c" => (bytes.len(), disk::checksum(bytes)) == (8078, 0xfdb2_d249),
                    "/src/kernel/task.c" => matches!((bytes.len(), disk::checksum(bytes)), (5587, 0xb041_d7c8) | (5698, 0x6ff2_a8b3)),
                    "/src/kernel/disk.c" => matches!((bytes.len(), disk::checksum(bytes)),
                        (8700, 0xa8ba_c0e5) | (20901, 0xf5e0_c618) | (20999, 0x7f64_447e)),
                    "/src/kernel/native.h" => matches!((bytes.len(), disk::checksum(bytes)),
                        (999, 0x055d_fc37) | (4091, 0xf3f4_5034) | (4367, 0x5e65_fa6c)),
                    "/src/kernel/display.c" => matches!((bytes.len(), disk::checksum(bytes)),
                        (4726, 0x739a_83ce) | (25975, 0xc6a3_fe7b) | (27711, 0x4571_61b1)),
                    "/src/kernel/proc.c" => matches!((bytes.len(), disk::checksum(bytes)), (3080, 0x5026_f57d) | (3094, 0x934c_4a47)),
                    "/src/kernel/kernel.c" => matches!((bytes.len(), disk::checksum(bytes)),
                        (1090, 0x4f0a_818c) | (2768, 0x69ee_92b2) | (2768, 0x7556_94ab)),
                    "/src/console.h" => matches!((bytes.len(), disk::checksum(bytes)), (569, 0x5646a81d)),
                    "/src/build/rebuild.c" => matches!((bytes.len(), disk::checksum(bytes)), (2668, 0xc7f2_1369) | (2761, 0x22fe_f9c0) | (2951, 0x9183_7f72) | (3094, 0x49c8_a199)),
                    "/src/core/text.h" => matches!((bytes.len(), disk::checksum(bytes)),
                        (12257, 0xf270_f24b) | (13019, 0x5b47_073d) | (13362, 0x31d5_48d4)),
                    "/src/core/commands.c" => matches!((bytes.len(), disk::checksum(bytes)),
                        (3511, 0x01eb_f07b) | (3760, 0x17da_1a6e) | (4597, 0x9ae8_b92f)
                        | (5354, 0xc3cd_3d9e) | (8010, 0xed04_3ba5) | (10663, 0xa550_c08e)),
                    "/src/tinycc/port.c" => matches!((bytes.len(), disk::checksum(bytes)), (8781, 0x182ee5f9) | (9991, 0xad0fa92c)),
                    "/src/tcc/port.c" => matches!((bytes.len(), disk::checksum(bytes)), (8781, 0x182ee5f9) | (9991, 0xad0fa92c)),
                    "/src/tinycc/slots.def" => matches!((bytes.len(), disk::checksum(bytes)), (2584, 0xc7c619ee)),
                    "/src/bash/bash.c" => matches!((bytes.len(), disk::checksum(bytes)),(21087, 0x5a27_2604)|(21217,0xd799627c)|(23905,0xf0d95640)|(24657,0x960d3663)|(28403,0x43e717dd)|(28478,0x1f9c15f8)|(28486,0x775d056a)),
                    "/src/ls/ls.c" => old_stock_ls_source(bytes),
                    "/src/core/discovery.h" => (bytes.len(), disk::checksum(bytes)) == (5547, 0x2cff_7a65),
                    "/src/links/config.h" => (bytes.len(), disk::checksum(bytes)) == (1058, 0xeca2a4c8),
                    "/src/links/load.h" => (bytes.len(), disk::checksum(bytes)) == (293, 0xec3d052c),
                    "/src/links/platform.c" => (bytes.len(), disk::checksum(bytes)) == (1612, 0xce9afe83),
                    "/src/links/prepare.py" => (bytes.len(), disk::checksum(bytes)) == (2504, 0x1c44bb5a),
                    "/src/links/window.c" => (bytes.len(), disk::checksum(bytes)) == (2372, 0x3f7daa1f),
                    "/src/links/Makefile" => matches!((bytes.len(), disk::checksum(bytes)), (1208, 0xc0a53c5c) | (1658,0x37e74e62)),
                    "/src/links/load.c" => matches!((bytes.len(), disk::checksum(bytes)), (6339, 0x199e4863) | (7009,0x36785356)),
                    "/src/links/port.c" => matches!((bytes.len(), disk::checksum(bytes)), (8914, 0x97f620b0) | (12503,0x0abe84af)),
                    "/src/links/COPYING" => matches!((bytes.len(), disk::checksum(bytes)), (18719, 0x0f0b0cd4) | (19830,0x8eaa86ec)),
                    "/src/ssh/port.c" => (bytes.len(), disk::checksum(bytes)) == (1810, 0xc6d26543),
                    "/src/ssh/src/lib.rs" => (bytes.len(), disk::checksum(bytes)) == (11370, 0x81c5b726),
                    "/src/desktop/desktop.c" => matches!((bytes.len(), disk::checksum(bytes)), (9781,0x5daec732) | (13586,0xb482580b) | (15502,0xa0d13781)),
                    "/src/desktop/font.swift" => (bytes.len(), disk::checksum(bytes)) == (1126,0xda7679a1),
                    "/src/foreground/nano.rs" => matches!((bytes.len(), disk::checksum(bytes)),
                        (8519, 0xc23c_73b8) | (15861, 0xea0a_62d3)),
                    "/src/agent/run.c" => old_stock_agent_source(bytes),
                    "/src/agent/README.md" => matches!((bytes.len(), disk::checksum(bytes)),
                        (1095, 0x0ddc_f2cf) | (1620, 0x2823_d007) | (1972, 0xc2ed_6bdd) | (2114, 0x1dcb_e514)),
                    "/src/agent/chat.md" => matches!((bytes.len(), disk::checksum(bytes)),
                        (1897, 0xaa32_1408) | (2681, 0x3bd1_dea5) | (3164, 0x868a_4295)
                        | (3268, 0x8140_54a4) | (3382, 0x97a7_59b4) | (4013, 0xc84e_5efc)
                        | (4411, 0x303a_d61a) | (5392, 0xb6f8_95d9)),
                    "/src/python/Makefile" => (bytes.len(), disk::checksum(bytes)) == (1043, 0xde44_90f8),
                    "/src/python/mpconfigport.h" | "/src/micropython/mpconfigport.h" =>
                        matches!((bytes.len(), disk::checksum(bytes)), (912, 0x3067_fe3d) | (1189, 0xef7d_fa2c)),
                    "/src/python/mphalport.h" | "/src/micropython/mphalport.h" =>
                        (bytes.len(), disk::checksum(bytes)) == (242, 0x1aa8_0e28),
                    "/src/python/port.c" | "/src/micropython/port.c" =>
                        matches!((bytes.len(), disk::checksum(bytes)), (1975, 0x9f70_3bdc) | (2834, 0x7532_1644)),
                    "/src/python/system.c" | "/src/micropython/system.c" =>
                        (bytes.len(), disk::checksum(bytes)) == (10903, 0xeb2e_454e),
                    "/src/micropython/sources.txt" => (bytes.len(), disk::checksum(bytes)) == (4214, 0x2357_d32d),
                    "/src/micropython/genhdr/moduledefs.h" => (bytes.len(), disk::checksum(bytes)) == (622, 0x7c78_205f),
                    "/src/micropython/genhdr/qstrdefs.generated.h" => (bytes.len(), disk::checksum(bytes)) == (9548, 0xaf3a_e6e4),
                    "/src/micropython/genhdr/root_pointers.h" => (bytes.len(), disk::checksum(bytes)) == (86, 0x8c0f_836f),
                    "/src/tcc/lixin.h" => (bytes.len(), disk::checksum(bytes)) == (1775, 0xf41f_67b3) || (bytes.len(),disk::checksum(bytes))==(2078,0x2c0d57c9),
                    "/src/tcc/api.c" => (bytes.len(),disk::checksum(bytes))==(3326,0x77cffc86),
                    "/src/tcc/slots.def" => matches!((bytes.len(), disk::checksum(bytes)), (2584, 0xc7c619ee)) || (bytes.len(),disk::checksum(bytes))==(1371,0x61f63c00),
                    "/src/sh/main.rs" => matches!((bytes.len(), disk::checksum(bytes)), (12860, 0xce52_7fea) | (16896, 0xee4c_2cce)),
                    "/src/sh/execute.rs" => matches!((bytes.len(), disk::checksum(bytes)),
                        (6893, 0xf0c3_d2b5) | (6898, 0xfbba_c8c9)),
                    "/src/sh/abi.rs" => (bytes.len(), disk::checksum(bytes)) == (530, 0x1613_cdd9) || (bytes.len(),disk::checksum(bytes))==(601,0x623df15f),
                    _ => false,
                });
                if upgrade {
                    let old = volume.files.insert(path.into(), contents.map(<[u8]>::to_vec));
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
                    !entry.as_deref().is_some_and(old_stock_ls_source)
                }),
                "/src/agent/run.c" => volume.files.get("/agent.c").filter(|entry| {
                    !entry.as_deref().is_some_and(old_stock_agent_source)
                }),
                _ => None,
            };
            let data = legacy.and_then(Option::as_deref).or(contents);
            let old = volume.files.insert(path.into(), data.map(<[u8]>::to_vec));
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
    };
    match volume.files.get("/bin") {
        None => {
            if let Err(err) = volume.commit("/bin".into(), Some(None), None) {
                print(&format!("Could not create /bin: {err}\n"));
            }
        }
        Some(Some(_)) => print("Existing /bin is a file; choose another PATH directory.\n"),
        Some(None) => (),
    }
    if volume.files.get("/bin") == Some(&None) {
        let old_bash = volume.files.get("/bin/bash").and_then(Option::as_deref)
            .is_some_and(|b| matches!((b.len(), disk::checksum(b)), (23720, 0xb2ca_0c34) | (23776, 0x77f085b2) | (26928, 0x8968_5afc) | (27976, 0x7c17_a489) | (31528, 0xd918_f074) | (31600, 0x89a2_a041) | (31608, 0x9d07_19bc)));
        if !volume.files.contains_key("/bin/bash") || old_bash {
            // Bootstrap the C shell with the same guest compiler users invoke.
            let built = crate::user::compile("/src/bash/bash.c", include_str!("../../User/bash/bash.c"))
                .and_then(|image| volume.commit("/bin/bash".into(), Some(Some(image)), None));
            if let Err(error) = built { print(&format!("Could not install Bash: {error}\n")); }
        }
        let old_shell = volume.files.get("/bin/sh").and_then(Option::as_deref)
            .is_some_and(|b| matches!((b.len(), disk::checksum(b)),
                (72656, 0xc93c_11f5 | 0x7df0_c3cb) | (93136, 0x65cc_454b))
                || b == include_bytes!("../target/user-build/sh.elf"));
        if !volume.files.contains_key("/bin/sh") || old_shell {
            let login: &[u8] = if volume.files.get("/bin/bash").and_then(Option::as_deref).is_some_and(runnable) {
                BASH_ALIAS
            } else {
                include_bytes!("../target/user-build/sh.elf")
            };
            if let Err(err) = volume.commit("/bin/sh".into(), Some(Some(login.to_vec())), None) {
                print(&format!("Could not install /bin/sh: {err}\n"));
            }
        }
        let agent_path = crate::agent_tools::PROGRAM_PATH;
        let old_agent = volume.files.get(agent_path).and_then(Option::as_deref)
            .is_some_and(|bytes| matches!((bytes.len(), disk::checksum(bytes)),
                (5320, 0x8df6_54f6) | (5568, 0xfb20_bfa9) | (5760, 0x79f4_37fc)
                | (5824, 0xc33f_35b8) | (9264, 0x91c1_0b0c)));
        if !volume.files.contains_key(agent_path) || old_agent {
            if let Err(error) = volume.commit(
                agent_path.into(),
                Some(Some(crate::agent_tools::PROGRAM.to_vec())),
                None,
            ) {
                print(&format!("Could not install {agent_path}: {error}\n"));
            }
        }
        // Compile these commands in the guest as LXE1. Replace only old stock launchers.
        for name in ["cp", "mv", "rm", "mkdir", "rmdir", "touch"] {
            let path = format!("/bin/{name}");
            let legacy = [LAUNCHER_PREFIX, name.as_bytes()].concat();
            if volume.files.get(&path).is_none_or(|entry| entry.as_deref() == Some(legacy.as_slice())) {
                let source = format!("#define APP {name}\n{}", include_str!("../../User/core/files.c"));
                let result = crate::user::compile("/src/core/files.c", &source)
                    .and_then(|image| volume.commit(path.clone(), Some(Some(image)), None));
                if let Err(error) = result { print(&format!("Could not install {path}: {error}\n")); }
            }
        }
        if !volume.files.contains_key("/bin/xargs") {
            let source = include_str!("../../User/core/xargs.c");
            if let Err(error) = crate::user::compile("/src/core/xargs.c", source)
                .and_then(|image| volume.commit("/bin/xargs".into(), Some(Some(image)), None))
            {
                print(&format!("Could not install /bin/xargs: {error}\n"));
            }
        }
        for name in COMMANDS {
            let path = format!("/bin/{name}");
            let old = volume.files.get(&path);
            let native = crate::utility::installed(name);
            let legacy = [LAUNCHER_PREFIX, name.as_bytes()].concat();
            let old_core = old.and_then(Option::as_deref).is_some_and(|bytes| old_core_elf(name, bytes));
            // Update the previous stock ELF while keeping guest-built replacements.
            let old_ls = *name == "ls" && old.and_then(Option::as_deref).is_some_and(|bytes| {
                matches!((bytes.len(), disk::checksum(bytes)),
                    (5896, 0x609e_7e97) | (6024, 0x18e2_b7e2) | (6056, 0x6724_d0b1)
                    | (7384, 0x926a_196f) | (8136, 0xd7b7_8e12))
            });
            let old_lust = *name == "lust" && old.and_then(Option::as_deref).is_some_and(|bytes| {
                matches!((bytes.len(), disk::checksum(bytes)),
                    (598112, 0x1a85_50b4) | (600320, 0xab2b_9bef) | (601104, 0x4b14_9bb2) | (606016, 0x0fdb_cc69) | (610544, 0x690c_2452) | (617888, 0x0846_68b7))
            });
            if old.is_none()
                || (native.is_some() && old.and_then(Option::as_deref) == Some(legacy.as_slice()))
                || (*name == "ssh" && old.and_then(Option::as_deref).is_some_and(|b|
                    matches!((b.len(), disk::checksum(b)), (134744, 0xe7907f77) | (142400, 0x8dfb7311))))
                || (*name == "links" && old.and_then(Option::as_deref).is_some_and(|b|
                    matches!((b.len(), disk::checksum(b)), (316056, 0x03004fc3) | (532008, 0x2514945e) | (801016, 0xe7615558))))
                || (*name == "stat" && old.and_then(Option::as_deref).is_some_and(|b|
                    (b.len(), disk::checksum(b)) == (20328, 0x61e3_c36a)))
                || old_core || old_ls || old_lust || (*name == "desktop" && old.and_then(Option::as_deref).is_some_and(|b|
                    matches!((b.len(), disk::checksum(b)), (55296, 0x9c478e70) | (59752, 0xb2dd3ede) | (60360, 0x5fc9_765f) | (60400, 0x50bc28a4) | (61176, 0x1ddd_ee18))))
            {
                let launcher = if *name == "links2" { LINKS_ALIAS.to_vec() } else { native.map_or(legacy, <[u8]>::to_vec) };
                if let Err(error) = volume.commit(path, Some(Some(launcher)), None) {
                    print(&format!("Could not install /bin/{name}: {error}\n"));
                    break;
                }
            }
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
        agent_bridge: Rc::new(RefCell::new(None)),
        started,
        cwd: "/".into(),
    };
    crate::scheduler::init();
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
                if let Some((code, data)) = crate::utility::load_segments(&image) {
                    crate::user::launch_segments(code, data, Some(("/", "")), false)
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
