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
use alloc::{collections::{BTreeMap, VecDeque}, format, rc::Rc, string::{String, ToString}, vec::Vec};
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
pub(crate) const PREVIEW_LINES: usize = 3;
pub(crate) const PREVIEW_OMITTED: &str = "[more lines hidden]";
pub(crate) const COMMAND_COLOR: &str = "\x1b[36m";
pub(crate) const COLOR_RESET: &str = "\x1b[0m";
pub(crate) const CANCELLED: &str = "project agent cancelled by operator";
const PAUSED: &str = "project agent paused by operator";
const CHAT_TURNS: usize = 8;
const CHAT_INPUT_HISTORY: usize = 64;
const CHAT_EXPORT_NAME: &str = "agent-conversation.txt";
const CHAT_INSTRUCTION: &str = include_str!("../../User/agent/chat.md");
const NEEDS_TESTS: &str = "every staged script must pass its test before review";

#[path = "../../User/foreground/keys.rs"]
mod terminal_keys;
#[path = "../../User/agent/chat_ui.rs"]
mod ui;
pub(crate) use ui::tool as print_tool;
#[path = "agent_report.rs"]
mod report;
#[path = "agent_trace.rs"]
mod trace;
pub(crate) use trace::list as list_traces;
use report::{Checks, Delivery, RunTarget};
pub(crate) use report::{ToolReport, ToolStatus};

pub(crate) fn print_colored(color: &str, text: &str) {
    print(color);
    print(text);
    print(COLOR_RESET);
}

fn chat_instruction(auto_approve: bool) -> String {
    let mode = if auto_approve {
        "\n\nAuto-approval is enabled for noninteractive exec commands in this session. \
         These commands can write immediately using normal guest permissions. \
         Terminal handoff and saving staged scripts still require operator confirmation."
    } else {
        ""
    };
    format!("{CHAT_INSTRUCTION}\n\n{}{mode}", crate::agent::PLATFORM_GUIDE)
}

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
    model_error: Option<String>,
    modern: bool,
    result: Option<String>,
    policy: String,
    conversation: Option<String>,
    auto_approve: bool,
    trace: Option<trace::Handle>,
}

struct Slot(UnsafeCell<Option<Turn>>);
// The single CPU runs one continuation at a time and each pane has its own slot.
unsafe impl Sync for Slot {}
static TURNS: [Slot; user::BANKS] = [const { Slot(UnsafeCell::new(None)) }; user::BANKS];

pub(crate) fn active() -> bool {
    unsafe { (*TURNS[user::bank()].0.get()).is_some() }
}

fn poll_operator() -> Option<&'static str> {
    if crate::scheduler::stopped() != 0 { return Some(CANCELLED); }
    match poll_byte() {
        Some(3 | 7) => Some(CANCELLED),
        Some(16) => Some(PAUSED),
        _ => None,
    }
}

fn operator_prompt(updates: &mut String, chat: &mut Option<ChatTurn<'_>>) -> Result<()> {
    print("\n");
    if chat.is_some() {
        ui::activity("Paused", None);
        ui::detail("Paused", "Enter a follow-up or press Enter to resume. Ctrl+G cancels.", ui::ATTENTION);
        ui::prompt();
    } else {
        print("agent paused. Enter a follow-up, or press Enter to resume. Ctrl+G cancels.\nagent> ");
    }
    let note = read_line(512.min(MAX_TEXT.saturating_sub(updates.len() + 1)), None)?.ok_or(CANCELLED)?;
    if !note.trim().is_empty() {
        updates.push_str(note.trim());
        updates.push('\n');
        if let Some(chat) = chat {
            writeln!(chat.transcript, "User follow-up: {}", visible(note.trim(), 512)).unwrap();
            chat.trace.borrow_mut().emit("follow_up", trace::text(note.trim(), 512));
        }
    }
    print("agent: resumed\n");
    Ok(())
}

fn redraw_chat_input(previous: &[u8], line: &[u8]) {
    let cols = ui::columns();
    let width = ui::PROMPT.len() + String::from_utf8_lossy(previous).chars().count();
    let wrapped = (width - 1) / cols.max(1);
    print("\r");
    if wrapped != 0 { print(&format!("\x1b[{wrapped}A")); }
    print("\x1b[J");
    ui::prompt();
    for &byte in line { put(byte); }
}

fn read_line(limit: usize, history: Option<&VecDeque<String>>) -> Result<Option<String>> {
    let mut line = Vec::new();
    let mut draft = Vec::new();
    let end = history.map_or(0, VecDeque::len);
    let mut position = end;
    let mut pending = None;
    loop {
        let first = pending.take().unwrap_or_else(read_byte);
        let key = terminal_keys::emacs_key(terminal_keys::decode(first, || {
            let byte = crate::read_timed_byte(100)?;
            if byte.is_ascii_control() {
                pending = Some(byte);
                None
            } else {
                Some(byte)
            }
        }));
        match key {
            3 | 7 => { print("\n"); return Err(CANCELLED); }
            4 if line.is_empty() => { print("\n"); return Ok(None); }
            b'\r' | b'\n' if history.is_some() && line.iter().all(u8::is_ascii_whitespace) => {
                if !line.is_empty() {
                    redraw_chat_input(&line, b"");
                    line.clear();
                }
            }
            b'\r' | b'\n' => break,
            16 | 14 if first == 27 && let Some(history) = history => {
                let next = if key == 16 { position.saturating_sub(1) } else { (position + 1).min(end) };
                if next == position { continue; }
                if position == end { draft.clone_from(&line); }
                position = next;
                let selected = if position == end { &draft[..] } else { history[position].as_bytes() };
                let mut size = selected.len().min(limit);
                while size < selected.len() && selected[size] & 0xc0 == 0x80 { size -= 1; }
                let selected = &selected[..size];
                redraw_chat_input(&line, selected);
                line.clear();
                line.extend_from_slice(selected);
            }
            8 | 127 if line.pop().is_some() => {
                if let Err(error) = core::str::from_utf8(&line) {
                    line.truncate(error.valid_up_to());
                }
                print("\x08 \x08");
            }
            byte @ (32..=126 | 128..=255) if line.len() < limit => {
                line.push(byte);
                put(byte);
            }
            _ => (),
        }
    }
    print("\n");
    Ok(Some(String::from_utf8_lossy(&line).into_owned()))
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
            // Chat prints one tool row when the broker has the actual action.
            let chat_header = turn.conversation.is_some() && text.starts_with("agent: [");
            if !chat_header {
                print(&visible(text, 4096));
            }
            0
        }
        1 if turn.requested && turn.action.is_none() => {
            let Some(text) = text.strip_suffix('\0') else {
                return u64::MAX;
            };
            let limit = if turn.conversation.is_some() { 65 } else { 4 };
            let fields: Vec<_> = text.splitn(limit + 1, '\0').map(String::from).collect();
            if fields.len() > limit {
                return u64::MAX;
            }
            turn.action = Some(fields);
            0
        }
        _ => u64::MAX,
    }
}

fn ask(turn: &mut Turn, instruction: String) -> Result<String> {
    let mut borrowed = turn.bridge.try_borrow_mut().map_err(|_| "agent is busy")?;
    let instruction = if turn.conversation.is_some() {
        chat_instruction(turn.auto_approve)
    } else {
        format!("{instruction}\n\n{}", crate::agent::PLATFORM_GUIDE)
    };
    let mut fields = alloc::vec![instruction.as_str()];
    fields.extend(turn.fields.iter().map(String::as_str));
    let action = if let Some(conversation) = &turn.conversation {
        fields.insert(7, conversation);
        "chat-session"
    } else {
        "project-session-context"
    };
    let mut request = |attempt, fields: &[&str]| {
        let remaining = turn.fields[2].parse().unwrap_or_default();
        if let Some(trace) = &turn.trace {
            trace.borrow_mut().emit("model_started", trace::Model { attempt, remaining, result: None });
        }
        let started = crate::monitor::ticks();
        let result = (|| {
            if borrowed.is_none() { *borrowed = Some(Bridge::connect()?); }
            let mut next_poll = 0;
            let poll_interval = crate::monitor::seconds_to_ticks(1) / 100;
            let mut waiting = (turn.conversation.is_some() && !ui::activity("Thinking", None))
                .then(ui::ModelWait::new);
            let result = borrowed.as_mut().unwrap().request_interruptible(action, fields, &mut || {
                let now = crate::monitor::ticks();
                if now < next_poll { return Ok(()); }
                next_poll = now + poll_interval;
                if let Some(waiting) = &mut waiting { waiting.tick(now); }
                poll_operator().map_or(Ok(()), Err)
            });
            if let Some(waiting) = waiting { waiting.clear(); }
            result
        })();
        let duration_ms = trace::elapsed_ms(started);
        let failure = borrowed.as_mut().and_then(Bridge::take_model_error);
        if let Some(trace) = &turn.trace {
            let status = trace::outcome(result.as_ref().map(|_| ()).map_err(|error| *error));
            let error = failure.as_ref().map(|error| visible(&error.detail, 512))
                .or_else(|| result.as_ref().err().map(|error| (*error).into()));
            trace.borrow_mut().emit("model_finished", trace::Model { attempt, remaining,
                result: Some(trace::Completion { status, duration_ms, detail: error.as_deref() }) });
        }
        (result, failure)
    };
    let (mut reply, mut failure) = request(1, &fields);
    if turn.conversation.is_some() && failure.as_ref().is_some_and(|error| error.retryable) {
        print("agent: retrying the model reply\n");
        let repair = format!("{instruction}\n\nRegenerate your reply as one JSON object \
            with exactly the two keys tool and args. Use an array of strings for args.");
        fields[0] = &repair;
        let (retried, retry_failure) = request(2, &fields);
        reply = retried;
        failure = retry_failure.or(failure);
    }
    turn.model_error = failure.map(|error| error.detail);
    let reply = reply?;
    let limit = if turn.conversation.is_some() && matches!(reply.first().map(String::as_str), Some("exec" | "terminal")) {
        65
    } else { 4 };
    if !(1..=limit).contains(&reply.len()) || reply.iter().any(|part| part.contains('\0')) {
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
    checks: Checks,
}

enum Review { Empty, Saved, Declined }

struct FailedCommand {
    reason: String,
    skipped_once: bool,
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

pub fn chat(
    root: &str, volume: Rc<RefCell<Volume>>, bridge: BridgeSlot, started: u64,
) -> Result<()> {
    if crate::scheduler::background() { return Err("agent chat requires a foreground terminal"); }
    if root == "/" || root == POLICY_ROOT || root.starts_with("/agent-policy/")
        || root == "/bin" || root.starts_with("/bin/") || in_git_metadata(root)
    {
        return Err("choose a project directory outside /bin, /agent-policy and .git");
    }
    let budget = {
        let volume = volume.borrow();
        if volume.files.get(root) != Some(&None) { return Err("project directory not found"); }
        policy_values(policy_source(&volume)).unwrap().0.parse().unwrap()
    };
    let mut auto_approve = true;
    let _header = ui::welcome(root, budget);
    let mut history = VecDeque::<String>::new();
    let mut session_id = None;
    let mut input_history = VecDeque::new();
    let mut log = ChatLog::default();
    loop {
        ui::activity("Ready", Some((0, budget)));
        ui::prompt();
        let task = match read_line(2048, Some(&input_history)) {
            Ok(Some(line)) if line.trim() == "/exit" => return Ok(()),
            Ok(Some(line)) => line,
            Ok(None) => return Ok(()),
            Err(CANCELLED) => continue,
            Err(error) => return Err(error),
        };
        let task = task.trim();
        if task.is_empty() { continue; }
        if input_history.back().map(String::as_str) != Some(task) {
            if input_history.len() == CHAT_INPUT_HISTORY { input_history.pop_front(); }
            input_history.push_back(task.into());
        }
        if task == "/trace" {
            list_traces(root, &volume.borrow())?;
            continue;
        }
        if task == "/bypass" {
            auto_approve = !auto_approve;
            ui::mode(auto_approve);
            continue;
        }
        if task == "/help" {
            ui::help();
            print("/trace lists recent structured traces in this project.\n");
            continue;
        }
        if task == "/prompt" {
            print(&chat_instruction(auto_approve));
            print("\n");
            continue;
        }
        if task == "/export" || task.starts_with("/export ") {
            match log.export(root, task["/export".len()..].trim(), &mut volume.borrow_mut()) {
                Ok(path) => print(&format!("agent: exported conversation to {path}\n")),
                Err(error) => print(&format!("agent: export failed: {error}\n")),
            }
            continue;
        }
        let conversation = history.iter().map(String::as_str).collect::<Vec<_>>().join("\n");
        let mut memory = String::new();
        let mut transcript = format!("User: {}\n", visible(task, 2048));
        let trace = trace::Trace::start(volume.clone(), root, task, &mut session_id, auto_approve);
        ui::activity("Thinking", Some((0, budget)));
        let outcome = run_session(root, task, volume.clone(), bridge.clone(), false,
            Some(ChatTurn { conversation: &conversation, memory: &mut memory, transcript: &mut transcript,
                started, auto_approve, trace: trace.clone() }));
        trace.borrow_mut().finish(outcome);
        match outcome {
            Ok(()) => (),
            Err(CANCELLED) => {
                let message = "turn cancelled. Discarded staged scripts.";
                print("\n");
                ui::detail("Cancelled", message, ui::ATTENTION);
                writeln!(transcript, "Result: {message}").unwrap();
            }
            Err(error) => {
                ui::detail("Error", error, ui::ERROR);
                writeln!(transcript, "Error: {error}").unwrap();
            }
        }
        log.push(transcript);
        if history.len() == CHAT_TURNS { history.pop_front(); }
        history.push_back(format!("User: {}\n{}", visible(task, 1024), visible(&memory, 2048)));
    }
}

#[derive(Default)]
struct ChatLog {
    turns: alloc::collections::VecDeque<String>,
    bytes: usize,
    omitted: usize,
}

impl ChatLog {
    // Leave room for the export header and a note about any evicted turns.
    const LIMIT: usize = 1024 * 1024 - 1024;

    fn push(&mut self, mut turn: String) {
        turn.push('\n');
        while self.bytes + turn.len() > Self::LIMIT {
            let Some(oldest) = self.turns.pop_front() else {
                self.omitted += 1;
                return;
            };
            self.bytes -= oldest.len();
            self.omitted += 1;
        }
        self.bytes += turn.len();
        self.turns.push_back(turn);
    }

    fn export(&self, root: &str, name: &str, volume: &mut Volume) -> Result<String> {
        if self.turns.is_empty() { return Err("no conversation to export yet"); }
        if volume.files.get(root) != Some(&None) { return Err("project directory not found"); }
        if name.contains('/') || matches!(name, "." | "..") || name.chars().any(char::is_control) {
            return Err("use a filename in the project directory");
        }
        let path = if name.is_empty() {
            let first = format!("{root}/{CHAT_EXPORT_NAME}");
            if volume.files.contains_key(&first) {
                (2..).map(|number| format!("{root}/agent-conversation-{number}.txt"))
                    .find(|candidate| !volume.files.contains_key(candidate)).unwrap()
            } else {
                first
            }
        } else {
            format!("{root}/{name}")
        };
        if path.len() > 512 { return Err("export path too long"); }
        if volume.files.contains_key(&path) { return Err("export file already exists"); }
        let mut content = format!("LixinOS Agent conversation\nProject: {root:?}\n\n");
        if self.omitted > 0 {
            writeln!(content, "[{} earlier turns omitted to fit the file size limit]\n", self.omitted).unwrap();
        }
        for turn in &self.turns { content.push_str(turn); }
        volume.commit(path.clone(), Some(Some(content.into_bytes())), None)?;
        Ok(path)
    }
}

struct ChatTurn<'a> {
    conversation: &'a str,
    memory: &'a mut String,
    transcript: &'a mut String,
    started: u64,
    auto_approve: bool,
    trace: trace::Handle,
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
    match run_session(root, task, volume, bridge, tuning, None) {
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
    mut chat: Option<ChatTurn<'_>>,
) -> Result<()> {
    if root == "/bin" || root.starts_with("/bin/") || in_git_metadata(root) {
        return Err("choose a project directory outside /bin and .git");
    }
    let (image, generation, policy, git_reference, budget) = {
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
        (image, volume.agent_revision(), format!("{mode}{steps} {preview}"),
            visible(crate::agent::git_reference(&volume.files), 2048), steps.parse::<usize>().unwrap())
    };
    if chat.is_none() {
        print(&format!("agent: starting /bin/agent-run in {root:?}\n"));
        print("Ctrl+P pauses for a follow-up. Ctrl+G cancels.\n");
    }
    let system = (root == "/").then(crate::agent_system::Plan::default);
    let mut project = Project { root, volume, generation, pending: BTreeMap::new(), tuning, system };
    let mut observations = Vec::<String>::new();
    let mut updates = String::new();
    let mut failed_commands = BTreeMap::<Vec<String>, FailedCommand>::new();
    let mut failure_scope = (generation, 0);
    let mut artifacts = Delivery::default();
    let mut changed_by_tools = false;
    let mut last_blocked_delivery = None;
    let mut memory = None;
    let mut delivery = None;
    for _ in 0..MAX_BROKER_STEPS * 2 {
        match poll_operator() {
            Some(PAUSED) => operator_prompt(&mut updates, &mut chat)?,
            Some(CANCELLED) => return Err(CANCELLED),
            _ => (),
        }
        let mut fields = alloc::vec![
            task.into(), root.into(), LEGACY_STEPS.saturating_sub(observations.len()).to_string(),
            updates.clone(), visible(&project.state(), 2560), git_reference.clone(),
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
                model_error: None,
                modern: false,
                result: delivery.take(),
                policy: policy.clone(),
                conversation: chat.as_ref().map(|turn| turn.conversation.into()),
                auto_approve: chat.as_ref().is_some_and(|turn| turn.auto_approve),
                trace: chat.as_ref().map(|turn| turn.trace.clone()),
            });
        }
        let status = if let Some(memory) = memory.take() {
            user::resume_agent(memory)
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
        if let Some(chat) = &mut chat && let Some(detail) = &turn.model_error {
            let label = if turn.error.is_some() { "Model error" } else { "Model reply retried" };
            writeln!(chat.transcript, "{label}: {}", visible(detail, 512)).unwrap();
        }
        match turn.error.or_else(poll_operator) {
            Some(PAUSED) => { operator_prompt(&mut updates, &mut chat)?; continue; }
            Some(error) => {
                if let Some(chat) = &mut chat {
                    *chat.memory = format!("Turn interrupted: {error}\n{}", recent_observations(&observations));
                }
                return Err(error);
            }
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
            let summary = visible(summary, MAX_TEXT);
            if chat.is_none() {
                print(&format!("agent: {summary}\n"));
                match project.review(None) {
                    Err(PAUSED) => { operator_prompt(&mut updates, &mut chat)?; continue; }
                    result => return result.map(|_| ()),
                }
            }
            let staged: Vec<_> = project.pending.keys().cloned().collect();
            let review = match project.review(Some(&mut artifacts)) {
                Err(PAUSED) => { operator_prompt(&mut updates, &mut chat)?; continue; }
                result => result?,
            };
            let chat = chat.as_mut().unwrap();
            let summary = if matches!(review, Review::Declined) {
                writeln!(chat.transcript, "Review: discarded staged scripts").unwrap();
                "The staged scripts were discarded."
            } else {
                if matches!(review, Review::Saved) {
                    writeln!(chat.transcript, "Review: applied staged scripts").unwrap();
                    for path in staged {
                        artifacts.verify(&project, &path, None, false)?;
                    }
                }
                &summary
            };
            let receipts = match artifacts.finalize(&project) {
                Ok(receipts) => receipts,
                Err((path, error)) => {
                    let limit = if turn.modern { budget } else { LEGACY_STEPS };
                    let detail = format!("Delivery blocked for {path:?}: {error}. Check the file and verify again.");
                    let report = ToolReport::new(ToolStatus::Stale, &detail).at(path);
                    let observation = report.observation(&action, false)?;
                    if observations.len() >= limit || last_blocked_delivery.as_ref() == Some(&observation) {
                        return Err(error);
                    }
                    print(&format!("agent: {detail}\n"));
                    writeln!(chat.transcript, "Tool finish []\nResult: {detail}").unwrap();
                    last_blocked_delivery = Some(observation.clone());
                    observations.push(observation);
                    delivery = Some("+".into());
                    continue;
                }
            };
            ui::reply(summary);
            chat.trace.borrow_mut().emit("assistant", trace::text(summary, MAX_TEXT));
            writeln!(chat.transcript, "Agent: {summary}").unwrap();
            let receipt_notes: Vec<_> = receipts.iter().map(|(path, receipt)| {
                print("\n");
                receipt.render(path);
                let line = receipt.describe(path);
                writeln!(chat.transcript, "{line}").unwrap();
                line
            }).collect();
            if receipts.is_empty() && changed_by_tools {
                let line = "The disk snapshot changed. This turn did not verify an artifact.";
                ui::detail("Delivery", line, ui::ATTENTION);
                writeln!(chat.transcript, "{line}").unwrap();
            }
            *chat.memory = format!("Agent: {}\n{}\n{}", visible(summary, 768),
                visible(&receipt_notes.join("\n"), 800), recent_observations(&observations));
            return Ok(());
        }
        let ceiling = if turn.modern { MAX_BROKER_STEPS } else { LEGACY_STEPS };
        if observations.len() >= ceiling {
            break;
        }
        let tool = action.first().map_or("", String::as_str);
        let command = matches!(tool, "exec" | "terminal") && chat.is_some();
        let step = observations.len() + 1;
        let target = trace::target(&action);
        if let Some(chat) = &chat {
            chat.trace.borrow_mut().emit("tool_started", trace::Tool { step, call_id: step,
                tool: &visible(tool, 32), target: target.as_deref(), duration_ms: None, result: None });
        }
        let tool_started = crate::monitor::ticks();
        if chat.is_some() {
            let status = match tool {
                "exec" => "Running",
                "terminal" => "Terminal",
                "test" | "smoke" => "Testing",
                "apply" => "Applying",
                "verify" => "Verifying",
                _ => "Using tool",
            };
            ui::activity(status, Some((step, if turn.modern { budget } else { LEGACY_STEPS })));
        }
        if !command && (!turn.modern || chat.is_some()) {
            let target = action.get(1).map_or_else(String::new, |operand| {
                format!(" {:?}", visible(operand, 160))
            });
            let title = format!("{}{target}", visible(tool, 32));
            if chat.is_some() {
                ui::tool(step, if turn.modern { budget } else { LEGACY_STEPS }, &title);
            } else {
                print(&format!("agent: [{step:02}/{LEGACY_STEPS}] {title}\n"));
            }
        }
        if let Some(chat) = &mut chat {
            let operands = if command { &action[1..] } else { &action[1..action.len().min(2)] };
            writeln!(chat.transcript, "Tool {} {}", visible(tool, 32),
                visible(&format!("{operands:?}"), MAX_TEXT)).unwrap();
        }
        let mut stopped_repeating = false;
        let outcome = if command {
            let limit = if turn.modern { budget } else { LEGACY_STEPS };
            let scope = (project.volume.borrow().agent_revision(), updates.len());
            if scope != failure_scope {
                failed_commands.clear();
                failure_scope = scope;
            }
            if let Some(previous) = failed_commands.get_mut(&action) {
                stopped_repeating = core::mem::replace(&mut previous.skipped_once, true);
                let target = visible(&format!("{:?}", &action[1..]), 512);
                ui::tool(step, limit, &format!("skipped repeated {tool} {target}"));
                Ok(ToolReport::new(ToolStatus::Skipped, format!(
                    "Skipped repeated failed command. Previous result: {}", previous.reason)))
            } else {
                let run_target = RunTarget::before(&project, &action[1..]);
                let outcome = run_command(&mut project, &action[1..], tool == "terminal",
                    (step, limit), chat.as_mut().unwrap());
                let after = project.volume.borrow().agent_revision();
                changed_by_tools |= tool == "exec" && scope.0 != after;
                if let Some(target) = run_target
                    && let Ok(report) = &outcome
                    && let Some(exit_code) = report.exit_code
                {
                    artifacts.record_run(target, exit_code, after);
                }
                let failure = match &outcome {
                    Ok(report) if report.status != ToolStatus::Ok => Some(report.detail.as_str()),
                    Err(error) if !matches!(*error, PAUSED | CANCELLED) => Some(*error),
                    _ => None,
                };
                if let Some(reason) = failure {
                    failed_commands.insert(action.clone(), FailedCommand {
                        reason: visible(reason.trim(), 512), skipped_once: false,
                    });
                }
                outcome
            }
        } else if chat.is_some() {
            project.chat_tool(&action, &mut artifacts)
        } else {
            project.tool(&action).map(|detail| ToolReport::new(ToolStatus::Ok, detail))
        };
        let (tag, mut report) = match outcome {
            Ok(report) => ('+', report),
            Err(PAUSED) => {
                operator_prompt(&mut updates, &mut chat)?;
                ('-', ToolReport::new(ToolStatus::Paused, if command {
                    "Command was not approved. Ask again if the follow-up requires it."
                } else {
                    "Tool paused before completion. Run it again before finishing."
                }))
            }
            Err(CANCELLED) => {
                if let Some(chat) = &chat {
                    let report = ToolReport::new(ToolStatus::Cancelled, CANCELLED);
                    chat.trace.borrow_mut().emit("tool_finished", trace::Tool { step, call_id: step, tool,
                        target: target.as_deref(), duration_ms: Some(trace::elapsed_ms(tool_started)), result: Some(&report) });
                }
                return Err(CANCELLED);
            }
            Err(error) => {
                if command { print(&format!("agent: command failed: {error}\n")); }
                let status = match error {
                    NEEDS_TESTS | "project script must be staged first" => ToolStatus::NeedsTests,
                    "file not found" | "project file not found" | "saved file not found" => ToolStatus::NotFound,
                    _ => ToolStatus::Failed,
                };
                ('-', ToolReport::new(status, format!("error: {error}")))
            }
        };
        let read_size = (tag == '+' && tool == "read").then_some(report.detail.len());
        changed_by_tools |= tool == "apply" && report.status == ToolStatus::Saved;
        report.detail = visible(&report.detail, MAX_TEXT);
        if let Some(chat) = &chat {
            chat.trace.borrow_mut().emit("tool_finished", trace::Tool { step, call_id: step, tool: &visible(tool, 32),
                target: target.as_deref(), duration_ms: Some(trace::elapsed_ms(tool_started)), result: Some(&report) });
        }
        let result = &report.detail;
        if let Some(chat) = &mut chat {
            writeln!(chat.transcript, "Result: {result}").unwrap();
            if matches!(tool, "apply") {
                let outcome = match report.status {
                    ToolStatus::Saved => Some("applied staged scripts"),
                    ToolStatus::Declined => Some("discarded staged scripts"),
                    _ => None,
                };
                if let Some(outcome) = outcome { writeln!(chat.transcript, "Review: {outcome}").unwrap(); }
            }
        }
        let styled_result = chat.is_some() && matches!(report.status,
            ToolStatus::TestPassed | ToolStatus::TestFailed | ToolStatus::TimedOut
        ) && !command;
        if styled_result {
            let color = if report.status == ToolStatus::TestPassed { ui::POSITIVE } else { ui::ERROR };
            print_colored(color, &preview_text(result, 512));
            print("\n");
        }
        if turn.modern {
            delivery = Some(match read_size {
                Some(bytes) => format!("r{bytes}\n{}", preview_text(&result, MAX_TEXT)),
                None if command || styled_result || report.status == ToolStatus::Verified => String::from("+"),
                None => format!("{tag}{result}"),
            });
        } else if !command && !styled_result {
            if let Some(bytes) = read_size {
                print(&format!("agent: read {bytes} bytes (preview)\n"));
            } else if tag == '+' && tool == "list" && result.is_empty() {
                print("agent: directory is empty\n");
            }
            if !result.is_empty() {
                let result = result.trim_end_matches('\n');
                let preview = if read_size.is_some() {
                    preview_text(result, 512)
                } else {
                    visible(result, 512)
                };
                print(&format!("{preview}\n"));
            }
        }
        observations.push(if chat.is_some() {
            report.observation(&action, command)?
        } else {
            visible(&format!("tool {:?}: {result}", visible(tool, 32)), MAX_TEXT)
        });
        if stopped_repeating && let Some(chat) = &mut chat {
            *chat.memory = recent_observations(&observations);
            return Err("stopped after repeated failed commands. Send a follow-up to continue.");
        }
    }
    Err("project agent reached its tool limit")
}

fn recent_observations(observations: &[String]) -> String {
    visible(&observations[observations.len().saturating_sub(3)..].join("\n"), 900)
}

fn run_command(
    project: &mut Project<'_>, argv: &[String], interactive: bool, progress: (usize, usize), chat: &mut ChatTurn<'_>,
) -> Result<ToolReport> {
    if argv.first().is_none_or(|command| command.is_empty())
        || argv.iter().map(|arg| arg.len() + 1).sum::<usize>() > 4096
        || argv[0].chars().any(char::is_control)
        || argv[1..].iter().any(|arg| arg.chars().any(|c| c.is_control() && !matches!(c, '\n' | '\t')))
    {
        return Err("expected a guest command and literal arguments, at most 4096 bytes");
    }
    let before = project.volume.borrow().agent_revision();
    let outcome = crate::process::agent_command(
        project.root, project.volume.clone(), chat.started, argv.to_vec(), interactive, false, progress, |full| {
            if chat.auto_approve && !interactive {
                writeln!(chat.transcript, "Approval: auto-approved guest command").unwrap();
                return Ok(true);
            }
            ui::activity("Approval", None);
            if full.is_some() { print("Enter v to view the full command.\n"); }
            loop {
                print_colored(ui::ATTENTION, "  Approval  ");
                print(if interactive {
                    "Hand over the terminal to this guest command? [y/N] "
                } else {
                    "Run this guest command with access to the guest filesystem? [y/N] "
                });
                let answer = read_line(32, None)?.ok_or(CANCELLED)?;
                match (answer.trim(), full) {
                    ("v" | "V", Some(full)) => print(&format!("{full}\n")),
                    ("y" | "Y", _) => {
                        ui::activity(if interactive { "Terminal" } else { "Running" }, None);
                        return Ok(true);
                    }
                    _ => return Ok(false),
                }
            }
        },
    );
    // Approved commands can write through their normal filesystem APIs. Never
    // rebase staged changes across writes performed outside their review.
    let after = project.volume.borrow().agent_revision();
    if after != before {
        if project.pending.is_empty() {
            project.generation = after;
        } else {
            print("agent: guest files changed; staged scripts must be reviewed in a new turn.\n");
        }
    }
    outcome
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

pub(crate) fn preview_lines(text: &str) -> &str {
    text.match_indices('\n').nth(PREVIEW_LINES - 1)
        .map_or(text, |(index, _)| &text[..index + 1])
}

fn preview_text(text: &str, limit: usize) -> String {
    let lines = preview_lines(text);
    let mut output = visible(lines, limit);
    if lines.len() < text.len() {
        if !output.ends_with('\n') { output.push('\n'); }
        output.push_str(PREVIEW_OMITTED);
    }
    output
}

fn review_confirmation(prompt: &str, chat: bool, mut preview: impl FnMut(bool) -> Result<()>) -> Result<bool> {
    preview(false)?;
    loop {
        print("Press v to expand the preview. Ctrl+P adds a follow-up.\n");
        if chat {
            ui::activity("Review", None);
            print_colored(ui::ATTENTION, "  Review  ");
        }
        print(prompt);
        let answer = read_byte();
        print("\n");
        match answer {
            3 | 7 => return Err(CANCELLED),
            16 => return Err(PAUSED),
            b'v' | b'V' => preview(true)?,
            b'y' | b'Y' => {
                if chat { ui::activity("Applying", None); }
                return Ok(true);
            }
            _ => return Ok(false),
        }
    }
}

impl Project<'_> {
    fn state(&self) -> String {
        if let Some(plan) = &self.system { return plan.summary(); }
        if self.pending.is_empty() {
            return "No scripts are staged.".into();
        }
        let mut state = String::new();
        for (path, candidate) in &self.pending {
            let status = if candidate.checks.failed != 0 {
                "test failed; repair needed"
            } else if candidate.checks.exact_passed != 0 {
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
        let path = volume.resolve(self.root, operand)?;
        let in_project = path == self.root
            || path.strip_prefix(self.root).is_some_and(|suffix| suffix.starts_with('/'));
        if !in_project || in_git_metadata(&path) {
            return Err("path outside the allowed project files");
        }
        Ok(path)
    }

    fn tool(&mut self, action: &[String]) -> Result<String> {
        if matches!(action, [tool] if tool == "system") {
            return Ok(crate::inspect::system());
        }
        if let Some(plan) = &mut self.system {
            return plan.tool(&self.volume.borrow(), action);
        }
        match action {
            [tool, operand] if tool == "list" => self.list(operand),
            [tool, operand] if tool == "read" => self.read(operand),
            [tool, operand, source] if tool == "write" => self.write(operand, source),
            [tool, operand, input, expected] if tool == "test" =>
                self.test(operand, input, expected, false, None).map(|report| report.detail),
            _ => Err("unsupported project tool or invalid arguments"),
        }
    }

    fn chat_tool(&mut self, action: &[String], delivery: &mut Delivery) -> Result<ToolReport> {
        match action {
            [tool, operand, input, expected] if matches!(tool.as_str(), "test" | "smoke") =>
                self.test(operand, input, expected, tool == "smoke", Some(delivery)),
            [tool] if tool == "apply" => {
                let paths = self.pending.keys().cloned().collect::<Vec<_>>();
                let result = match self.review(Some(delivery))? {
                    Review::Empty => ToolReport::new(ToolStatus::Ok, "No scripts are staged."),
                    Review::Declined => ToolReport::new(ToolStatus::Declined, "Operator discarded the staged scripts."),
                    Review::Saved => ToolReport::new(ToolStatus::Saved, format!("Saved {paths:?}. Verify the saved files before delivery.")),
                };
                Ok(result)
            }
            [tool, operand, options @ ..] if tool == "verify" => {
                let (launch, interactive) = match options {
                    [] => (None, false),
                    [launch] => (Some(launch.as_str()), false),
                    [launch, mode] if mode == "interactive" => (Some(launch.as_str()), true),
                    _ => return Err("use verify(path, optional launch command, optional interactive)"),
                };
                let path = self.path(operand)?;
                if self.pending.contains_key(&path) {
                    return Ok(ToolReport::new(ToolStatus::Staged, "File has staged changes. Use apply before verifying.").at(path));
                }
                delivery.verify(self, &path, launch, interactive)
            }
            [tool, operand, ..] if matches!(tool.as_str(), "read" | "write" | "list") => {
                let detail = self.tool(action)?;
                let path = self.path(operand)?;
                let status = if self.pending.contains_key(&path) { ToolStatus::Staged } else { ToolStatus::Ok };
                Ok(ToolReport::new(status, detail).at(path))
            }
            _ => self.tool(action).map(|detail| ToolReport::new(ToolStatus::Ok, detail)),
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
            source: source.into(), checks: Checks::default(),
        });
        Ok(format!("staged {path:?}. Run the test tool against this candidate"))
    }

    fn test(&mut self, operand: &str, input: &str, expected: &str, smoke: bool,
        delivery: Option<&mut Delivery>,
    ) -> Result<ToolReport> {
        let path = self.path(operand)?;
        if input.len() > 1024 || expected.len() > user::TEST_OUTPUT_LIMIT {
            return Err("project test data exceeds its size limit");
        }
        if smoke && expected.is_empty() { return Err("provide an expected output marker for smoke"); }
        let staged = self.pending.get_mut(&path);
        if smoke && staged.is_some() {
            return Ok(ToolReport::new(ToolStatus::Staged, "Use apply to save before running a smoke test.").at(path));
        }
        let source = if let Some(candidate) = &staged {
            candidate.source.clone()
        } else if delivery.is_some() && path.ends_with(".py") {
            let bytes = self.volume.borrow().read_file(&path)?;
            String::from_utf8(bytes).map_err(|_| "saved Python script is not UTF-8")?
        } else {
            return Err("project script must be staged first");
        };
        let (status, output) = user::test_python_interruptible(&path, &source, input, poll_operator);
        if status == Err(PAUSED) {
            if let Some(candidate) = staged { candidate.checks.exact_passed = 0; }
            return Err(PAUSED);
        }
        if status == Err(CANCELLED) {
            return Err(CANCELLED);
        }
        let decoded = core::str::from_utf8(&output);
        let actual = String::from_utf8_lossy(&output).replace("\r\n", "\n");
        let matched = if smoke { actual.contains(expected) } else { actual == expected };
        let passed = status.is_ok() && decoded.is_ok() && matched;
        if let Some(candidate) = staged {
            candidate.checks.record(smoke, passed);
        } else if let Some(delivery) = delivery {
            delivery.record_check(path.clone(), source.into_bytes(), smoke, passed);
        }
        let detail = if passed && smoke {
            format!("smoke passed for {path:?}: scripted input matched {expected:?}, and the program exited successfully")
        } else if passed {
            format!("test passed for {path:?} with input {input:?} and output {actual:?}")
        } else if smoke {
            format!("smoke failed for {path:?}: {status:?}; expected marker {expected:?}, got {actual:?}")
        } else {
            format!("test failed for {path:?}: {status:?}; expected {expected:?}, got {actual:?}")
        };
        let outcome = if passed { ToolStatus::TestPassed } else if status == Err("test timed out") {
            ToolStatus::TimedOut
        } else { ToolStatus::TestFailed };
        let mut report = ToolReport::new(outcome, detail).at(path);
        report.exit_code = status.is_ok().then_some(0);
        Ok(report)
    }

    fn review(&mut self, delivery: Option<&mut Delivery>) -> Result<Review> {
        if let Some(plan) = &mut self.system {
            if plan.is_empty() { return Ok(Review::Empty); }
            if !review_confirmation("Apply these system filesystem changes? [y/N] ", false, |expanded| {
                let volume = self.volume.borrow();
                if volume.agent_revision() != self.generation {
                    return Err("filesystem changed during the system task");
                }
                plan.preview(&volume, if expanded { visible } else { preview_text });
                Ok(())
            })? {
                print("agent: discarded staged system changes\n");
                return Ok(Review::Declined);
            }
            let mut volume = self.volume.borrow_mut();
            if volume.agent_revision() != self.generation {
                return Err("filesystem changed during review");
            }
            plan.apply(&mut volume)?;
            print("agent: applied system changes. Use agent system-rollback to undo this batch.\n");
            return Ok(Review::Saved);
        }
        if self.pending.is_empty() {
            return Ok(Review::Empty);
        }
        if self.pending.values().any(|candidate| candidate.checks.exact_passed == 0 || candidate.checks.failed != 0) {
            return Err(NEEDS_TESTS);
        }
        if !review_confirmation("Apply the staged scripts to disk? [y/N] ", delivery.is_some(), |expanded| {
            let volume = self.volume.borrow();
            if volume.agent_revision() != self.generation {
                return Err("filesystem changed during the project task");
            }
            let preview = if expanded { visible } else { preview_text };
            for (path, candidate) in &self.pending {
                print(&format!("--- current {path:?}\n"));
                if self.tuning {
                    print(policy_source(&volume));
                    print("\n");
                } else if let Some(bytes) = volume.files.get(path).and_then(Option::as_deref) {
                    print(&preview(&String::from_utf8_lossy(bytes), MAX_TEXT));
                    print("\n");
                }
                print(&format!("+++ proposed {path:?}\n"));
                print(&preview(&candidate.source, MAX_TEXT));
                print("\n");
            }
            Ok(())
        })? {
            self.pending.clear();
            print("agent: discarded staged scripts\n");
            return Ok(Review::Declined);
        }
        let mut volume = self.volume.borrow_mut();
        if volume.agent_revision() != self.generation {
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
            return Ok(Review::Saved);
        }
        let new_files = self.pending.keys().filter(|path| !volume.files.contains_key(*path)).count();
        if volume.files.len() + new_files > ENTRY_LIMIT {
            return Err("filesystem full");
        }
        let mut undo = Vec::new();
        for (path, candidate) in &self.pending {
            let old = volume.files.insert(path.clone(), Some(candidate.source.as_bytes().to_vec()));
            undo.push((path.clone(), old));
        }
        if let Err(error) = volume.save_changes(undo.iter().map(|(path, old)| (path.as_str(), old.is_some())), &crate::disk::Times::new()) {
            for (path, old) in undo {
                if let Some(old) = old {
                    volume.files.insert(path, old);
                } else {
                    volume.files.remove(&path);
                }
            }
            return Err(error);
        }
        self.generation = volume.agent_revision();
        let saved = core::mem::take(&mut self.pending);
        if let Some(delivery) = delivery {
            for (path, candidate) in saved {
                delivery.record_checks(path, candidate.source.into_bytes(), candidate.checks);
            }
        }
        print("agent: applied staged scripts\n");
        Ok(Review::Saved)
    }
}

fn policy_test(source: &str) -> TestCase {
    let (steps, preview) = policy_values(source).unwrap();
    TestCase { input: String::new(), output: format!("{steps} {preview}\n") }
}
