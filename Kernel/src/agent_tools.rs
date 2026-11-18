//! Privileged project tools for the disk-resident EL0 agent. The broker owns
//! path checks, test results, and the commit; the executable owns its loop policy.
use crate::{
    agent::{Bridge, Revision, TestCase, Version},
    disk::{self, ENTRY_LIMIT},
    poll_byte, print, put, read_byte,
    process::Volume,
    user,
    utility,
};
use alloc::{collections::BTreeMap, format, rc::Rc, string::{String, ToString}, vec::Vec};
use core::{
    cell::{RefCell, UnsafeCell},
    fmt::Write,
    slice,
};

type Result<T> = core::result::Result<T, &'static str>;
type BridgeSlot = Rc<RefCell<Option<Bridge>>>;
const LEGACY_STEPS: usize = 50;
const MAX_BROKER_STEPS: usize = 60;
const MAX_FILES: usize = 4;
const MAX_TEXT: usize = 4096;
const CANCELLED: &str = "project agent cancelled by operator";
const PAUSED: &str = "project agent paused by operator";

pub const PROGRAM: &[u8] = include_bytes!("../target/user-build/agent-run.elf");
pub const PROGRAM_PATH: &str = "/bin/agent-run";
pub const POLICY_ROOT: &str = "/agent-policy";
pub const POLICY_PATH: &str = "/agent-policy/settings.py";
const DEFAULT_POLICY: &str = include_str!("../../User/agent/settings.py");

fn policy_values(source: &str) -> Option<(&str, &str)> {
    let (steps, preview) = source.trim().strip_prefix("print(\"")?
        .strip_suffix("\")")?.split_once(' ')?;
    let decimal = |value: &str| {
        if value.len() <= 4 && value.bytes().all(|byte| byte.is_ascii_digit()) {
            value.parse::<usize>().ok()
        } else {
            None
        }
    };
    ((1..=MAX_BROKER_STEPS).contains(&decimal(steps)?)
        && (64..=2048).contains(&decimal(preview)?)).then_some((steps, preview))
}

fn policy_source(volume: &Volume) -> &str {
    volume.files.get(POLICY_PATH).and_then(Option::as_deref)
        .and_then(|bytes| core::str::from_utf8(bytes).ok())
        .filter(|source| policy_values(source).is_some())
        .unwrap_or(DEFAULT_POLICY)
}

struct Turn {
    bridge: BridgeSlot,
    fields: Vec<String>,
    requested: bool,
    action: Option<Vec<String>>,
    error: Option<&'static str>,
    modern: bool,
    result: Option<String>,
    policy: String,
}

struct Slot(UnsafeCell<Option<Turn>>);
// The single CPU runs one continuation at a time and each pane has its own slot.
unsafe impl Sync for Slot {}
static TURNS: [Slot; user::BANKS] = [const { Slot(UnsafeCell::new(None)) }; user::BANKS];

pub(crate) fn active() -> bool {
    unsafe { (*TURNS[user::bank()].0.get()).is_some() }
}

fn poll_operator() -> Option<&'static str> {
    match poll_byte() {
        Some(3 | 7) => Some(CANCELLED),
        Some(16) => Some(PAUSED),
        _ => None,
    }
}

fn operator_prompt(updates: &mut String) -> Result<()> {
    print("\nagent paused. Enter a follow-up, or press Enter to resume. Ctrl+G cancels.\nagent> ");
    let mut note = String::new();
    loop {
        match read_byte() {
            3 | 7 => { print("\n"); return Err(CANCELLED); }
            b'\r' | b'\n' => break,
            8 | 127 if note.pop().is_some() => print("\x08 \x08"),
            byte @ 32..=126 if note.len() < 512 && updates.len() + note.len() < MAX_TEXT - 1 => {
                note.push(char::from(byte));
                put(byte);
            }
            _ => (),
        }
    }
    print("\n");
    if !note.trim().is_empty() {
        updates.push_str(note.trim());
        updates.push('\n');
    }
    print("agent: resumed\n");
    Ok(())
}

pub fn exchange(value: u64) -> u64 {
    let Some(turn) = (unsafe { (&mut *TURNS[user::bank()].0.get()).as_mut() }) else {
        return u64::MAX;
    };
    let length = (value >> 8) as usize;
    let operation = value & 255;
    let deliver = |text: &str| {
        let destination = unsafe { slice::from_raw_parts_mut(user::git_mailbox(), text.len()) };
        destination.copy_from_slice(text.as_bytes());
        text.len() as u64
    };
    match (operation, length) {
        (3, 0) => return turn.result.take().map_or(0, |result| deliver(&result)),
        (5, 0) => return deliver(&turn.policy),
        _ => (),
    }
    if !(1..=user::SCRIPT_CAPACITY).contains(&length) {
        return u64::MAX;
    }
    // This bank's private mailbox stays mapped until the EL0 turn returns.
    let bytes = unsafe { slice::from_raw_parts(user::git_mailbox(), length) };
    let Ok(text) = core::str::from_utf8(bytes) else {
        return u64::MAX;
    };
    match operation {
        0 | 2 if !turn.requested => {
            let instruction = if operation == 2 {
                let Some((remaining, instruction)) = text.split_once('\0') else {
                    return u64::MAX;
                };
                let Ok(remaining) = remaining.parse::<usize>() else { return u64::MAX };
                if remaining > MAX_BROKER_STEPS { return u64::MAX; }
                turn.fields[2] = remaining.to_string();
                turn.modern = true;
                instruction
            } else {
                text
            };
            if instruction.len() > MAX_TEXT || instruction.contains('\0') {
                return u64::MAX;
            }
            turn.requested = true;
            let reply = match ask(turn, instruction.into()) {
                Ok(reply) => reply,
                Err(PAUSED) => {
                    turn.error = Some(PAUSED);
                    // Let the harness yield its continuation in the usual way.
                    String::from("pause\0")
                }
                Err(error) => {
                    turn.error = Some(error);
                    return u64::MAX;
                }
            };
            deliver(&reply)
        }
        4 if length <= 4096 => {
            print(&visible(text, 4096));
            0
        }
        1 if turn.requested && turn.action.is_none() => {
            let Some(text) = text.strip_suffix('\0') else {
                return u64::MAX;
            };
            // At most a tool name and three arguments.
            let fields: Vec<_> = text.splitn(5, '\0').map(String::from).collect();
            if fields.len() > 4 {
                return u64::MAX;
            }
            turn.action = Some(fields);
            0
        }
        _ => u64::MAX,
    }
}

fn ask(turn: &Turn, instruction: String) -> Result<String> {
    let mut borrowed = turn.bridge.try_borrow_mut().map_err(|_| "agent is busy")?;
    if borrowed.is_none() {
        *borrowed = Some(Bridge::connect()?);
    }
    let mut fields = alloc::vec![instruction.as_str()];
    fields.extend(turn.fields.iter().map(String::as_str));
    let mut next_poll = 0;
    let poll_interval = crate::monitor::seconds_to_ticks(1) / 100;
    let reply = borrowed.as_mut().unwrap().request_interruptible("project-session", &fields, || {
        let now = crate::monitor::ticks();
        if now < next_poll { return Ok(()); }
        next_poll = now + poll_interval;
        poll_operator().map_or(Ok(()), Err)
    })?;
    if !(1..=4).contains(&reply.len()) || reply.iter().any(|part| part.contains('\0')) {
        return Err("invalid project tool reply");
    }
    if reply[0] == "pause" {
        return Err("model requested a reserved project control action");
    }
    let mut packed = reply.join("\0");
    packed.push('\0');
    if packed.len() > user::SCRIPT_CAPACITY {
        return Err("project tool reply too large");
    }
    Ok(packed)
}

struct Candidate {
    source: String,
    passed: bool,
    failed: bool,
}

struct Project<'a> {
    root: &'a str,
    volume: Rc<RefCell<Volume>>,
    generation: u64,
    pending: BTreeMap<String, Candidate>,
    tuning: bool,
    system: Option<crate::agent_system::Plan>,
}

pub fn run(
    root: &str,
    task: &str,
    volume: Rc<RefCell<Volume>>,
    bridge: BridgeSlot,
) -> Result<()> {
    if root == "/" { return Err("use agent system for tasks spanning the guest filesystem"); }
    if root == POLICY_ROOT || root.starts_with("/agent-policy/") {
        return Err("use agent tune for the agent policy");
    }
    launch(root, task, volume, bridge, false)
}

pub fn system(task: &str, volume: Rc<RefCell<Volume>>, bridge: BridgeSlot) -> Result<()> {
    print("agent: system mode. Change proposals can affect any guest directory.\n");
    launch("/", task, volume, bridge, false)
}

pub fn tune(task: &str, volume: Rc<RefCell<Volume>>, bridge: BridgeSlot) -> Result<()> {
    {
        let mut volume = volume.borrow_mut();
        match volume.files.get(POLICY_ROOT) {
            None => volume.commit(POLICY_ROOT.into(), Some(None), None)?,
            Some(None) => (),
            Some(Some(_)) => return Err("agent policy directory is occupied by a file"),
        }
    }
    launch(POLICY_ROOT, task, volume, bridge, true)
}

fn launch(
    root: &str, task: &str, volume: Rc<RefCell<Volume>>, bridge: BridgeSlot, tuning: bool,
) -> Result<()> {
    match run_session(root, task, volume, bridge, tuning) {
        Err(CANCELLED) => {
            print("\nagent: cancelled. Discarded staged scripts.\n");
            Ok(())
        }
        result => result,
    }
}

fn run_session(
    root: &str,
    task: &str,
    volume: Rc<RefCell<Volume>>,
    bridge: BridgeSlot,
    tuning: bool,
) -> Result<()> {
    if root == "/bin" || root.starts_with("/bin/") || in_git_metadata(root) {
        return Err("choose a project directory outside /bin and .git");
    }
    let (image, generation, policy) = {
        let volume = volume.borrow();
        if volume.files.get(root) != Some(&None) {
            return Err("project directory not found");
        }
        let image = volume.files.get(PROGRAM_PATH)
            .and_then(Option::as_deref)
            .filter(|image| utility::segment(image).is_some() || user::is_executable(image))
            .ok_or("invalid /bin/agent-run executable")?
            .to_vec();
        let (steps, preview) = policy_values(policy_source(&volume)).unwrap();
        let mode = if root == "/" { 's' } else if tuning { 't' } else { 'p' };
        (image, volume.disk.snapshot_status().1, format!("{mode}{steps} {preview}"))
    };
    print(&format!("agent: starting /bin/agent-run in {root:?}\n"));
    print("Ctrl+P pauses for a follow-up. Ctrl+G cancels.\n");
    let system = (root == "/").then(crate::agent_system::Plan::default);
    let mut project = Project { root, volume, generation, pending: BTreeMap::new(), tuning, system };
    let mut observations = Vec::<String>::new();
    let mut updates = String::new();
    let mut memory: Option<Vec<u8>> = None;
    let mut delivery = None;
    for _ in 0..MAX_BROKER_STEPS * 2 {
        match poll_operator() {
            Some(PAUSED) => operator_prompt(&mut updates)?,
            Some(CANCELLED) => return Err(CANCELLED),
            _ => (),
        }
        let mut fields = alloc::vec![
            task.into(), root.into(), LEGACY_STEPS.saturating_sub(observations.len()).to_string(),
            updates.clone(), project.state(),
        ];
        fields.extend(observations.iter().cloned());
        let slot = TURNS[user::bank()].0.get();
        unsafe {
            *slot = Some(Turn {
                bridge: bridge.clone(),
                fields,
                requested: false,
                action: None,
                error: None,
                modern: false,
                result: delivery.take(),
                policy: policy.clone(),
            });
        }
        let status = if let Some(memory) = memory.take() {
            user::resume_agent(&image, &memory)
        } else if let Some(code) = utility::segment(&image) {
            user::launch_status(code, None, false)
        } else {
            user::start_saved(&image, 1, "agent-run\0", 0)
        };
        let turn = unsafe { (*slot).take().unwrap() };
        if (tuning || root == "/") && turn.requested && !turn.modern {
            return Err("this operation requires the current /bin/agent-run");
        }
        if status == Ok(256) && turn.action.is_some() {
            memory = Some(user::suspend_process());
        }
        if turn.modern && status == Ok(2) && turn.error.is_none() {
            return Err("project agent reached its tool limit");
        }
        match turn.error.or_else(poll_operator) {
            Some(PAUSED) => { operator_prompt(&mut updates)?; continue; }
            Some(error) => return Err(error),
            None => (),
        }
        // Older guest builds exited after each turn. They can still use the broker.
        if !matches!(status?, 0 | 256) {
            return Err("agent-run exited unsuccessfully");
        }
        let action = turn.action.ok_or("agent-run did not submit a tool request")?;
        if let [tool, summary] = action.as_slice()
            && tool == "finish"
        {
            print(&format!("agent: {}\n", visible(summary, MAX_TEXT)));
            match project.review() {
                Err(PAUSED) => { operator_prompt(&mut updates)?; continue; }
                result => return result,
            }
        }
        let ceiling = if turn.modern { MAX_BROKER_STEPS } else { LEGACY_STEPS };
        if observations.len() >= ceiling {
            break;
        }
        let tool = action.first().map_or("", String::as_str);
        if !turn.modern {
            let target = action.get(1).map_or_else(String::new, |operand| {
                format!(" {:?}", visible(operand, 160))
            });
            print(&format!(
                "agent: [{:02}/{LEGACY_STEPS}] {}{target}\n",
                observations.len() + 1, visible(tool, 32),
            ));
        }
        let (tag, result) = match project.tool(&action) {
            Ok(result) => ('+', result),
            Err(PAUSED) => {
                operator_prompt(&mut updates)?;
                ('-', String::from("test paused before completion. Run it again before review"))
            }
            Err(CANCELLED) => return Err(CANCELLED),
            Err(error) => ('-', format!("error: {error}")),
        };
        let read_size = (tag == '+' && tool == "read").then_some(result.len());
        let result = visible(&result, MAX_TEXT);
        if turn.modern {
            delivery = Some(match read_size {
                Some(bytes) => format!("r{bytes}\n{result}"),
                None => format!("{tag}{result}"),
            });
        } else {
            if let Some(bytes) = read_size {
                print(&format!("agent: read {bytes} bytes (preview)\n"));
            } else if tag == '+' && tool == "list" && result.is_empty() {
                print("agent: directory is empty\n");
            }
            if !result.is_empty() {
                print(&format!("{}\n", visible(result.trim_end_matches('\n'), 512)));
            }
        }
        let observation = format!("tool {:?}: {result}", visible(tool, 32));
        observations.push(visible(&observation, MAX_TEXT));
    }
    Err("project agent reached its tool limit")
}

fn in_git_metadata(path: &str) -> bool {
    path.split('/').any(|part| part.eq_ignore_ascii_case(".git"))
}

fn visible(text: &str, limit: usize) -> String {
    let mut output = String::new();
    for c in text.chars() {
        if output.len() + c.len_utf8() > limit {
            output.push_str("\n[truncated]");
            break;
        }
        if !c.is_control() || matches!(c, '\n' | '\t') {
            output.push(c);
        }
    }
    output
}

impl Project<'_> {
    fn state(&self) -> String {
        if let Some(plan) = &self.system { return plan.summary(); }
        if self.pending.is_empty() {
            return "No scripts are staged.".into();
        }
        let mut state = String::new();
        for (path, candidate) in &self.pending {
            let status = if candidate.failed {
                "test failed; repair needed"
            } else if candidate.passed {
                "tests passed"
            } else {
                "staged; needs a test"
            };
            writeln!(state, "{path:?}: {status}").unwrap();
        }
        state
    }

    fn path(&self, operand: &str) -> Result<String> {
        if operand.chars().any(char::is_control) {
            return Err("invalid project path");
        }
        let volume = self.volume.borrow();
        let path = disk::resolve_path(&volume.files, self.root, operand)?;
        let in_project = path == self.root
            || path.strip_prefix(self.root).is_some_and(|suffix| suffix.starts_with('/'));
        if !in_project || in_git_metadata(&path) {
            return Err("path outside the allowed project files");
        }
        Ok(path)
    }

    fn tool(&mut self, action: &[String]) -> Result<String> {
        if let Some(plan) = &mut self.system {
            return plan.tool(&self.volume.borrow(), action);
        }
        match action {
            [tool, operand] if tool == "list" => self.list(operand),
            [tool, operand] if tool == "read" => self.read(operand),
            [tool, operand, source] if tool == "write" => self.write(operand, source),
            [tool, operand, input, expected] if tool == "test" => self.test(operand, input, expected),
            _ => Err("unsupported project tool or invalid arguments"),
        }
    }

    fn list(&self, operand: &str) -> Result<String> {
        let path = self.path(operand)?;
        let volume = self.volume.borrow();
        if volume.files.get(&path) != Some(&None) {
            return Err("project directory not found");
        }
        let mut listing = String::new();
        for (name, entry) in disk::children(&volume.files, &path) {
            if !name.eq_ignore_ascii_case(".git") {
                let kind = if entry.is_some() { "file" } else { "directory" };
                writeln!(listing, "{kind} {name:?}").unwrap();
            }
        }
        if self.tuning && path == POLICY_ROOT && !volume.files.contains_key(POLICY_PATH) {
            writeln!(listing, "file \"settings.py\" (default)").unwrap();
        }
        for pending in self.pending.keys() {
            let (parent, name) = pending.rsplit_once('/').unwrap();
            if parent == path {
                writeln!(listing, "staged {name:?}").unwrap();
            }
        }
        Ok(listing)
    }

    fn read(&self, operand: &str) -> Result<String> {
        let path = self.path(operand)?;
        if let Some(candidate) = self.pending.get(&path) {
            return Ok(candidate.source.clone());
        }
        let volume = self.volume.borrow();
        if self.tuning && path == POLICY_PATH {
            return Ok(policy_source(&volume).into());
        }
        let bytes = volume.files.get(&path)
            .and_then(Option::as_deref)
            .ok_or("project file not found")?;
        core::str::from_utf8(bytes).map(String::from).map_err(|_| "binary project file")
    }

    fn write(&mut self, operand: &str, source: &str) -> Result<String> {
        let path = self.path(operand)?;
        if !path.ends_with(".py") || source.trim().is_empty() || source.len() > MAX_TEXT
            || source.contains('\0')
        {
            return Err("stage a nonempty .py file of at most 4096 bytes");
        }
        if self.tuning && (path != POLICY_PATH || policy_values(source).is_none()) {
            return Err("stage only settings.py with print(\"STEPS PREVIEW\"): steps 1–60, preview 64–2048");
        }
        let volume = self.volume.borrow();
        if !self.tuning && volume.versions.contains_key(&path) {
            return Err("use agent improve for a versioned script");
        }
        if let Some(entry) = volume.files.get(&path) {
            let bytes = entry.as_deref().ok_or("is a directory")?;
            core::str::from_utf8(bytes).map_err(|_| "binary project file")?;
            if bytes.len() > MAX_TEXT {
                return Err("existing script exceeds the review size limit");
            }
        }
        if self.pending.len() == MAX_FILES && !self.pending.contains_key(&path) {
            return Err("project change limit reached");
        }
        if self.pending.get(&path).is_some_and(|candidate| candidate.source == source) {
            return Ok(format!("{path:?} is already staged with identical content. Use its current test status to choose the next tool"));
        }
        self.pending.insert(path.clone(), Candidate {
            source: source.into(), passed: false, failed: false,
        });
        Ok(format!("staged {path:?}. Run the test tool against this candidate"))
    }

    fn test(&mut self, operand: &str, input: &str, expected: &str) -> Result<String> {
        let path = self.path(operand)?;
        if input.len() > 1024 || expected.len() > user::TEST_OUTPUT_LIMIT {
            return Err("project test data exceeds its size limit");
        }
        let candidate = self.pending.get_mut(&path).ok_or("project script must be staged first")?;
        let (status, output) = user::test_python_interruptible(&path, &candidate.source, input, poll_operator);
        if status == Err(PAUSED) {
            candidate.passed = false;
            return Err(PAUSED);
        }
        if status == Err(CANCELLED) {
            return Err(CANCELLED);
        }
        let decoded = core::str::from_utf8(&output);
        let actual = String::from_utf8_lossy(&output).replace("\r\n", "\n");
        let passed = status.is_ok() && decoded.is_ok() && actual == expected;
        candidate.passed |= passed;
        candidate.failed |= !passed;
        let result = if passed {
            format!("test passed for {path:?} with input {input:?} and output {actual:?}")
        } else {
            format!("test failed for {path:?}: {status:?}; expected {expected:?}, got {actual:?}")
        };
        Ok(result)
    }

    fn review(&mut self) -> Result<()> {
        if let Some(plan) = &mut self.system {
            if plan.is_empty() { return Ok(()); }
            {
                let volume = self.volume.borrow();
                if volume.disk.snapshot_status().1 != self.generation {
                    return Err("filesystem changed during the system task");
                }
                plan.preview(&volume, visible);
            }
            print("Ctrl+P adds a follow-up before applying.\nApply these system filesystem changes? [y/N] ");
            let answer = read_byte();
            print("\n");
            if matches!(answer, 3 | 7) { return Err(CANCELLED); }
            if answer == 16 { return Err(PAUSED); }
            if !matches!(answer, b'y' | b'Y') {
                print("agent: discarded staged system changes\n");
                return Ok(());
            }
            let mut volume = self.volume.borrow_mut();
            if volume.disk.snapshot_status().1 != self.generation {
                return Err("filesystem changed during review");
            }
            plan.apply(&mut volume)?;
            print("agent: applied system changes. Use agent system-rollback to undo this batch.\n");
            return Ok(());
        }
        if self.pending.is_empty() {
            return Ok(());
        }
        if self.pending.values().any(|candidate| !candidate.passed || candidate.failed) {
            return Err("every staged script must pass its test before review");
        }
        {
            let volume = self.volume.borrow();
            if volume.disk.snapshot_status().1 != self.generation {
                return Err("filesystem changed during the project task");
            }
            for (path, candidate) in &self.pending {
                print(&format!("--- current {path:?}\n"));
                if self.tuning {
                    print(policy_source(&volume));
                    print("\n");
                } else if let Some(bytes) = volume.files.get(path).and_then(Option::as_deref) {
                    print(&visible(&String::from_utf8_lossy(bytes), MAX_TEXT));
                    print("\n");
                }
                print(&format!("+++ proposed {path:?}\n"));
                print(&visible(&candidate.source, MAX_TEXT));
                print("\n");
            }
        }
        print("Ctrl+P adds a follow-up before applying.\nApply the staged scripts to disk? [y/N] ");
        let answer = read_byte();
        print("\n");
        if matches!(answer, 3 | 7) { return Err(CANCELLED); }
        if answer == 16 { return Err(PAUSED); }
        if !matches!(answer, b'y' | b'Y') {
            print("agent: discarded staged scripts\n");
            return Ok(());
        }
        let mut volume = self.volume.borrow_mut();
        if volume.disk.snapshot_status().1 != self.generation {
            return Err("filesystem changed during review");
        }
        if self.tuning {
            let candidate = self.pending.remove(POLICY_PATH).ok_or("missing staged agent settings")?;
            let previous = policy_source(&volume);
            let version = Version {
                tests: alloc::vec![policy_test(&candidate.source)],
                previous: Some(Revision { source: previous.into(), tests: alloc::vec![policy_test(previous)] }),
            };
            volume.commit(POLICY_PATH.into(), Some(Some(candidate.source.into_bytes())), Some(version))?;
            print("agent: applied settings for the next session\n");
            return Ok(());
        }
        let new_files = self.pending.keys().filter(|path| !volume.files.contains_key(*path)).count();
        if volume.files.len() + new_files > ENTRY_LIMIT {
            return Err("filesystem full");
        }
        let mut undo = Vec::new();
        for (path, candidate) in core::mem::take(&mut self.pending) {
            let old = volume.files.insert(path.clone(), Some(candidate.source.into_bytes()));
            undo.push((path, old));
        }
        let Volume { disk, files, versions } = &mut *volume;
        if let Err(error) = disk.save(files, versions) {
            for (path, old) in undo {
                if let Some(old) = old {
                    files.insert(path, old);
                } else {
                    files.remove(&path);
                }
            }
            return Err(error);
        }
        print("agent: applied staged scripts\n");
        Ok(())
    }
}

fn policy_test(source: &str) -> TestCase {
    let (steps, preview) = policy_values(source).unwrap();
    TestCase { input: String::new(), output: format!("{steps} {preview}\n") }
}
