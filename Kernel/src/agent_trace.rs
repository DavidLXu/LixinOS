//! Guest-side JSONL events for one interactive agent turn or operator action.
use super::{Result, ToolReport, visible};
use crate::{monitor, print, process::Volume, utility};
use alloc::{format, rc::Rc, string::String};
use core::cell::RefCell;
use serde::Serialize;

const PREFIX: &str = ".agent-trace-";
const SUFFIX: &str = ".jsonl";

pub(super) type Handle = Rc<RefCell<Trace>>;

pub(super) struct Trace {
    volume: Rc<RefCell<Volume>>,
    root: String,
    session_id: Option<String>,
    path: Option<String>,
    started: u64,
    sequence: u32,
    disabled: bool,
}

pub(super) fn elapsed_ms(start: u64) -> u64 {
    monitor::ticks().saturating_sub(start).saturating_mul(1_000) / monitor::seconds_to_ticks(1)
}

pub(super) fn outcome(result: Result<()>) -> &'static str {
    match result {
        Ok(()) => "ok",
        Err(super::CANCELLED) => "cancelled",
        Err(super::PAUSED) => "paused",
        Err("project agent reached its tool limit") => "tool_limit",
        Err(_) => "failed",
    }
}

#[derive(Serialize)]
struct Record<'a, T> {
    schema: u8,
    seq: u32,
    elapsed_ms: u64,
    event: &'static str,
    session_id: &'a str,
    turn_id: &'a str,
    data: T,
}

#[derive(Serialize)]
pub(super) struct Completion<'a> {
    pub(super) status: &'a str,
    pub(super) duration_ms: u64,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(super) detail: Option<&'a str>,
}

#[derive(Serialize)]
pub(super) struct Model<'a> {
    pub(super) attempt: u8,
    pub(super) remaining: usize,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(super) result: Option<Completion<'a>>,
}

#[derive(Serialize)]
pub(super) struct Tool<'a> {
    pub(super) step: usize,
    pub(super) call_id: usize,
    pub(super) tool: &'a str,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(super) target: Option<&'a str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(super) duration_ms: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(super) result: Option<&'a ToolReport>,
}

pub(super) fn target(action: &[String]) -> Option<String> {
    let tool = action.first()?.as_str();
    let index = match tool {
        "exec" | "terminal" | "read" | "list" | "write" | "test" | "smoke" | "verify" => 1,
        _ => return None,
    };
    Some(visible(action.get(index)?, 256))
}

impl Trace {
    pub(super) fn start(
        volume: Rc<RefCell<Volume>>, root: &str, task: &str,
        session_id: &mut Option<String>, auto_approve: bool,
    ) -> Handle {
        let mut trace = Self {
            volume, root: root.into(), session_id: session_id.clone(),
            path: None, started: monitor::ticks(), sequence: 0, disabled: false,
        };
        #[derive(Serialize)]
        struct Start<'a> {
            project: &'a str,
            mode: &'a str,
            task: &'a str,
            unix_seconds: u32,
            auto_approve: bool,
        }
        trace.emit("turn_started", Start {
            project: root, mode: "chat",
            task: &visible(task, 2048), unix_seconds: utility::wall_seconds(), auto_approve,
        });
        session_id.clone_from(&trace.session_id);
        Rc::new(RefCell::new(trace))
    }

    pub(super) fn emit<T: Serialize>(&mut self, event: &'static str, data: T) {
        if !self.disabled && let Err(error) = self.append(event, data) {
            self.disabled = true;
            print(&format!("agent: trace stopped: {error}\n"));
        }
    }

    pub(super) fn finish(&mut self, result: Result<()>) {
        let duration_ms = elapsed_ms(self.started);
        self.emit("turn_finished", Completion { status: outcome(result), duration_ms, detail: result.err() });
    }

    fn append<T: Serialize>(&mut self, event: &'static str, data: T) -> Result<()> {
        let mut volume = self.volume.borrow_mut();
        let path = match &self.path {
            Some(path) => path.clone(),
            None => {
                let base = format!("{}/{PREFIX}{:010}-", self.root, utility::wall_seconds());
                if base.len() + 16 > 512 { return Err("trace path too long"); }
                (1..=9999).map(|n| format!("{base}{n:04}{SUFFIX}"))
                    .find(|path| !volume.files.contains_key(path))
                    .ok_or("no available trace filename")?
            }
        };
        let turn_id = path.rsplit('/').next().unwrap_or(&path);
        let session_id = self.session_id.as_deref().unwrap_or(turn_id);
        let record = Record { schema: 2, seq: self.sequence + 1,
            elapsed_ms: elapsed_ms(self.started), event, session_id, turn_id, data };
        let session_id = String::from(session_id);
        let mut line = serde_json_wasm::to_vec(&record).map_err(|_| "could not encode trace event")?;
        line.push(b'\n');
        let mut bytes = if self.path.is_some() {
            volume.files.get(&path).and_then(Option::as_ref).ok_or("trace file was removed")?.clone()
        } else {
            alloc::vec::Vec::new()
        };
        if bytes.len() + line.len() > 512 * 1024 { return Err("trace size limit reached"); }
        bytes.extend_from_slice(&line);
        volume.commit_agent_trace(path.clone(), bytes)?;
        self.path = Some(path);
        self.session_id = Some(session_id);
        self.sequence += 1;
        Ok(())
    }
}

pub(super) fn text(value: &str, limit: usize) -> Text {
    Text { text: visible(value, limit) }
}

#[derive(Serialize)]
pub(super) struct Text { text: String }

pub(crate) fn list(root: &str, volume: &Volume) -> Result<()> {
    if volume.files.get(root) != Some(&None) { return Err("project directory not found"); }
    let mut latest = alloc::vec::Vec::new();
    for (name, entry) in crate::disk::children(&volume.files, root) {
        if entry.is_some() && name.starts_with(PREFIX) && name.ends_with(SUFFIX) {
            latest.push(name);
        }
    }
    if latest.is_empty() {
        print("agent: no traces in this project\n");
    } else {
        for name in latest.into_iter().rev().take(8) {
            print(&format!("{}/{}\n", if root == "/" { "" } else { root }, name));
        }
    }
    Ok(())
}
