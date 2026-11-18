#[path = "process_io.rs"]
mod io;
use crate::{
    agent::{self, Revision, Version, Versions},
    print,
};
use alloc::{collections::VecDeque, format, string::String, vec::Vec};
pub(crate) use io::{active, exchange};
#[cfg(feature = "split")]
pub(crate) use io::ready_on_read;

use alloc::rc::Rc;
use core::cell::RefCell;

type Result<T> = core::result::Result<T, &'static str>;
use crate::disk::{self, ENTRY_LIMIT, FILE_LIMIT, HISTORY_LIMIT};
const COMMANDS: &[&str] = &[
    "agent", "cat", "cd", "cecho", "clear", "desktop", "echo", "git", "grep", "help", "htop", "kilo", "ls",
    "lust", "mkdir", "nano", "netinfo", "paint", "pwd", "python", "rm", "ssh", "shutdown", "tcc", "touch", "wc",
];

// A system launcher names a handler supplied by the running kernel.
const LAUNCHER_PREFIX: &[u8] = b"LXcmd1\n";

fn system_handler(bytes: &[u8]) -> Option<&'static str> {
    let name = bytes.strip_prefix(LAUNCHER_PREFIX)?;
    COMMANDS.iter().copied().find(|cmd| cmd.as_bytes() == name)
}

// A disk-resident login alias follows replacements of /bin/bash. Writing
// a real executable to /bin/sh still explicitly overrides the login program.
const BASH_ALIAS: &[u8] = b"LXalias1\n/bin/bash";

fn runnable(bytes: &[u8]) -> bool {
    bytes == BASH_ALIAS || system_handler(bytes).is_some()
        || crate::utility::segment(bytes).is_some()
        || crate::user::is_executable(bytes)
        || crate::user::process_image(bytes).is_some()
}

pub(crate) struct Volume {
    pub(crate) disk: crate::disk::Disk,
    pub(crate) files: crate::disk::Files,
    pub(crate) versions: Versions,
}

impl Volume {
    // A missing entry removes the path. A present entry contains a file or a directory.
    pub(crate) fn commit(
        &mut self,
        path: String,
        entry: Option<Option<Vec<u8>>>,
        version: Option<Version>,
    ) -> Result<()> {
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
        if let Err(error) = self.disk.save(&self.files, &self.versions) {
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
        disk::resolve_path(&self.volume.borrow().files, &self.cwd, name)
    }

    fn store(&mut self, path: String, value: Option<String>) -> Result<()> {
        self.store_bytes(path, value.map(String::into_bytes))
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
            ("cpu", "") => Ok(inspect::cpu(self.started)),
            ("memory", "") => Ok(inspect::memory()),
            ("disk", "") => Ok(inspect::disk(&self.volume.borrow().disk)),
            ("filesystem", "") => Ok(inspect::filesystem(&self.volume.borrow().files)),
            ("list" | "stat", path) if !path.is_empty() => {
                let path = self.path(path)?;
                let volume = self.volume.borrow();
                if tool == "list" {
                    inspect::list(&volume.files, &path)
                } else {
                    inspect::stat(&volume.files, &path)
                }
            }
            _ => Err("unsupported inspection tool or invalid path argument"),
        }
    }
    fn bytes(&self, name: &str) -> Result<Vec<u8>> {
        self.volume
            .borrow()
            .files
            .get(&self.path(name)?)
            .ok_or("file not found")?
            .clone()
            .ok_or("is a directory")
    }

    fn executable_bytes(&self, name: &str) -> Result<Vec<u8>> {
        let image = self.bytes(name)?;
        if image == BASH_ALIAS { self.bytes("/bin/bash") } else { Ok(image) }
    }

    fn mutate(&mut self, cmd: &str, args: &[String]) -> Result<()> {
        let arg = || match args {
            [name] => Ok(name.as_str()),
            _ => Err("expected one argument"),
        };
        match cmd {
            "touch" if args.len() == 2 && args[0] == "--files-from" => {
                let names = self.file(&args[1])?;
                let mut borrowed = self.volume.borrow_mut();
                let volume = &mut *borrowed;
                let mut added = Vec::new();
                let result = (|| {
                    for name in names.lines().filter(|name| !name.is_empty()) {
                        let path = disk::resolve_path(&volume.files, &self.cwd, name)?;
                        if volume.files.contains_key(&path) {
                            continue;
                        }
                        if volume.files.len() == ENTRY_LIMIT {
                            return Err("filesystem full");
                        }
                        volume.files.insert(path, Some(Vec::new()));
                        added.push(name);
                    }
                    if !added.is_empty() {
                        volume.disk.save(&volume.files, &volume.versions)?;
                    }
                    Ok(())
                })();
                if let Err(err) = result {
                    for name in added.into_iter().rev() {
                        let path = disk::resolve_path(&volume.files, &self.cwd, name)?;
                        volume.files.remove(&path);
                    }
                    return Err(err);
                }
                Ok(())
            }
            "mkdir" | "touch" | "rm" => {
                let path = self.path(arg()?)?;
                match cmd {
                    "mkdir" if self.volume.borrow().files.contains_key(&path) => {
                        return Err("already exists");
                    }
                    "mkdir" => self.store(path, None)?,
                    "touch" if !self.volume.borrow().files.contains_key(&path) => {
                        self.store(path, Some(String::new()))?
                    }
                    "rm" => {
                        self.bytes(arg()?)?;
                        self.volume.borrow_mut().commit(path, None, None)?;
                    }
                    _ => (),
                }
                Ok(())
            }
            _ => Err("unknown operation"),
        }
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
                return self.with_agent(|bridge| {
                    crate::agent::inspect(bridge, question, &self.cwd, |tool, path| {
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
                    let image = crate::user::compile(src, &self.file(src)?)?;
                    return self.store_bytes(dest, Some(image));
                }
                _ => return Err("usage: tcc -run source.c | tcc source.c -o output"),
            }
        }
        if cmd == "python" {
            let script = match tokens {
                [_] => None,
                [_, path] => Some((path.as_str(), self.file(path)?)),
                _ => return Err("usage: python [script.py]"),
            };
            return crate::user::run(
                crate::user::PYTHON,
                script.as_ref().map(|(path, text)| (*path, text.as_str())),
            );
        }
        self.mutate(cmd, &tokens[1..])
    }
}

fn old_stock_ls_source(bytes: &[u8]) -> bool {
    matches!((bytes.len(), disk::checksum(bytes)),
        (2533, 0xae82_4921) | (2790, 0x5ee0_c525))
}

fn old_stock_agent_source(bytes: &[u8]) -> bool {
    matches!((bytes.len(), disk::checksum(bytes)),
        (1482, 0xb54d_2c61) | (1538, 0x8270_ae19) | (1821, 0x3298_95ab) | (7457, 0x06a8_d956))
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
                    "/src/bash/bash.c" => (bytes.len(), disk::checksum(bytes)) == (21087, 0x5a27_2604),
                    "/src/ls/ls.c" => old_stock_ls_source(bytes),
                    "/src/desktop/desktop.c" => (bytes.len(), disk::checksum(bytes)) == (9781,0x5daec732),
                    "/src/desktop/font.swift" => (bytes.len(), disk::checksum(bytes)) == (1126,0xda7679a1),
                    "/src/agent/run.c" => old_stock_agent_source(bytes),
                    "/src/tcc/lixin.h" => (bytes.len(), disk::checksum(bytes)) == (1775, 0xf41f_67b3),
                    "/src/sh/main.rs" => (bytes.len(), disk::checksum(bytes)) == (12860, 0xce52_7fea),
                    "/src/sh/execute.rs" => matches!((bytes.len(), disk::checksum(bytes)),
                        (6893, 0xf0c3_d2b5) | (6898, 0xfbba_c8c9)),
                    "/src/sh/abi.rs" => (bytes.len(), disk::checksum(bytes)) == (530, 0x1613_cdd9),
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
            volume.disk.save(&volume.files, &volume.versions)?;
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
            .is_some_and(|b| (b.len(), disk::checksum(b)) == (23720, 0xb2ca_0c34));
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
                (5320, 0x8df6_54f6) | (5568, 0xfb20_bfa9) | (5824, 0xc33f_35b8) | (9264, 0x91c1_0b0c)));
        if !volume.files.contains_key(agent_path) || old_agent {
            if let Err(error) = volume.commit(
                agent_path.into(),
                Some(Some(crate::agent_tools::PROGRAM.to_vec())),
                None,
            ) {
                print(&format!("Could not install {agent_path}: {error}\n"));
            }
        }
        for name in COMMANDS {
            let path = format!("/bin/{name}");
            let old = volume.files.get(&path);
            let native = crate::utility::installed(name);
            let legacy = [LAUNCHER_PREFIX, name.as_bytes()].concat();
            // Update the previous stock ELF while keeping guest-built replacements.
            let old_ls = *name == "ls" && old.and_then(Option::as_deref).is_some_and(|bytes| {
                matches!((bytes.len(), disk::checksum(bytes)),
                    (5896, 0x609e_7e97) | (6024, 0x18e2_b7e2))
            });
            let old_lust = *name == "lust" && old.and_then(Option::as_deref).is_some_and(|bytes| {
                matches!((bytes.len(), disk::checksum(bytes)),
                    (598112, 0x1a85_50b4) | (600320, 0xab2b_9bef))
            });
            if old.is_none()
                || (native.is_some() && old.and_then(Option::as_deref) == Some(legacy.as_slice()))
                || old_ls || old_lust || (*name == "desktop" && old.and_then(Option::as_deref).is_some_and(|b| (b.len(),disk::checksum(b)) == (55296,0x9c478e70)))
            {
                let launcher = native.map_or(legacy, <[u8]>::to_vec);
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
    let services = Services {
        volume: Rc::new(RefCell::new(volume)),
        agent_bridge: Rc::new(RefCell::new(None)),
        started,
        cwd: "/".into(),
    };
    #[cfg(feature = "split")]
    {
        let other = services.clone();
        let initial = history.clone();
        crate::panes::init(alloc::boxed::Box::new(move || session(other, initial)));
    }
    session(services, history)
}

fn session(services: Services, history: VecDeque<String>) -> ! {
    let image = services.executable_bytes("/bin/sh").ok();
    let result = match image {
        Some(image) if crate::user::process_image(&image).is_some() || crate::user::is_executable(&image) => {
            io::run(services, history, image, cfg!(feature = "split"))
        }
        Some(image) => {
            // A locally compiled replacement can also serve as the login program.
            crate::utility::with_io("/", services.volume.clone(), None, false, || {
                if let Some(code) = crate::utility::segment(&image) {
                    crate::user::launch(code, Some(("/", "")), false)
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
    crate::idle()
}
