// Interactive client for the guest console and the in-process Codex app server.
use super::*;
use codex_app_server_protocol::{ThreadItemPayload, TurnStatus};
use std::collections::VecDeque;
use std::io::{self, Write};
use std::time::Duration;

unsafe extern "C" {
    fn lx_codex_terminal() -> i32;
    fn lx_timed_byte(ms: u32) -> i32;
}

const HELP: &str = "Enter sends a message. Up/Down recall prompts. Ctrl-C cancels. /exit or Ctrl-D leaves.";
const PROMPT: &str = "you> ";

pub struct Console {
    pending: VecDeque<u8>,
    history: VecDeque<String>,
    streaming: Option<(String, String)>,
}

fn printable(text: &str) -> String {
    text.chars().filter(|c| !c.is_control() || matches!(c, '\n' | '\t')).collect()
}

fn key() -> Option<u8> {
    u8::try_from(unsafe { lx_timed_byte(0) }).ok()
}

impl Console {
    pub fn open() -> anyhow::Result<Self> {
        anyhow::ensure!(unsafe { lx_codex_terminal() } == 1,
                        "interactive Codex needs a terminal; use codex exec for pipes");
        eprintln!("Codex {} | LixinOS\n{HELP}", env!("CARGO_PKG_VERSION"));
        Ok(Self { pending: VecDeque::new(), history: VecDeque::new(), streaming: None })
    }

    pub(super) async fn interrupted(&mut self) {
        loop {
            while let Some(byte) = key() {
                if byte == 3 {
                    self.pending.clear();
                    return;
                }
                if self.pending.len() < 8192 {
                    self.pending.push_back(byte);
                }
            }
            tokio::time::sleep(Duration::from_millis(30)).await;
        }
    }

    async fn byte(&mut self) -> u8 {
        loop {
            if let Some(byte) = self.pending.pop_front().or_else(key) {
                return byte;
            }
            tokio::time::sleep(Duration::from_millis(20)).await;
        }
    }

    async fn line(&mut self, label: &str, recall: bool) -> io::Result<Option<String>> {
        let mut bytes = Vec::new();
        let mut draft = Vec::new();
        let mut index = self.history.len();
        let mut escape = 0;
        eprint!("{label}");
        loop {
            let b = self.byte().await;
            if escape == 1 && b == b'[' {
                escape = 2;
                continue;
            }
            if escape == 2 {
                if (0x40..=0x7e).contains(&b) {
                    escape = 0;
                    if recall && matches!(b, b'A' | b'B') {
                        if index == self.history.len() { draft = bytes.clone(); }
                        index = if b == b'A' { index.saturating_sub(1) }
                                else { (index + 1).min(self.history.len()) };
                        bytes = self.history.get(index).map_or_else(|| draft.clone(), |s| s.as_bytes().to_vec());
                        eprint!("\r\x1b[2K{label}{}", String::from_utf8_lossy(&bytes));
                    }
                }
                continue;
            }
            escape = 0;
            match b {
                27 => escape = 1,
                3 | 4 => { eprintln!(); return Ok(None); }
                b'\r' | b'\n' => { eprintln!(); return Ok(Some(String::from_utf8_lossy(&bytes).into_owned())); }
                8 | 127 if !bytes.is_empty() => {
                    while bytes.pop().is_some_and(|last| last & 0xc0 == 0x80) {}
                    eprint!("\r\x1b[2K{label}{}", String::from_utf8_lossy(&bytes));
                }
                32..=126 | 128..=255 if bytes.len() < 8192 => {
                    bytes.push(b);
                    io::stderr().write_all(&[b])?;
                }
                _ => {}
            }
        }
    }

    pub async fn prompt(&mut self) -> io::Result<Option<String>> {
        loop {
            let Some(line) = self.line(PROMPT, true).await? else { return Ok(None); };
            match line.trim() {
                "" => continue,
                "/help" => eprintln!("{HELP}"),
                "/exit" | "/quit" => return Ok(None),
                _ => {
                    if self.history.len() == 32 { self.history.pop_front(); }
                    self.history.push_back(line.clone());
                    return Ok(Some(line));
                }
            }
        }
    }

    pub(super) async fn handle_request(
        &mut self, client: &AppServerClient, request: ServerRequest, error_seen: &mut bool,
    ) -> anyhow::Result<()> {
        use codex_app_server_protocol::CommandExecutionApprovalDecision as Decision;
        let method = server_request_method_name(&request);
        let (id, details, can_accept) = match request {
            ServerRequest::CommandExecutionRequestApproval { request_id, params } => {
                let allowed = params.available_decisions.as_ref()
                    .is_none_or(|options| options.contains(&Decision::Accept));
                (request_id, serde_json::to_string_pretty(&params)?, allowed)
            }
            ServerRequest::FileChangeRequestApproval { request_id, params } =>
                (request_id, serde_json::to_string_pretty(&params)?, true),
            other => {
                handle_server_request(client, other, error_seen).await;
                return Ok(());
            }
        };
        eprintln!("Approval requested: {method}\n{}", printable(&details));
        // Typeahead for a future chat turn must never grant a pending approval.
        self.pending.clear();
        while key().is_some() {}
        let accept = can_accept && self.line("Allow once? [y/N] ", false).await?
            .is_some_and(|s| s.trim().eq_ignore_ascii_case("y"));
        let value = serde_json::json!({"decision": if accept { "accept" } else { "decline" }});
        resolve_server_request(client, id, value, &method).await.map_err(anyhow::Error::msg)
    }

    pub(super) fn render(&mut self, event: ServerNotification) -> CodexStatus {
        match event {
            ServerNotification::AgentMessageDelta(n) => {
                let delta = printable(&n.delta);
                let (id, shown) = self.streaming.get_or_insert_with(|| {
                    eprint!("codex> ");
                    (n.item_id.clone(), String::new())
                });
                if *id != n.item_id {
                    eprint!("\ncodex> ");
                    *id = n.item_id;
                    shown.clear();
                }
                eprint!("{delta}");
                shown.push_str(&delta);
            }
            ServerNotification::ItemStarted(n) => {
                if let ThreadItemPayload::CommandExecution { command, .. } = n.item.payload {
                    eprintln!("$ {}", printable(&command));
                }
            }
            ServerNotification::ItemCompleted(n) => match n.item.payload {
                ThreadItemPayload::AgentMessage { id: item_id, text, .. } => {
                    let text = printable(&text);
                    if let Some((id, shown)) = self.streaming.take()
                        && id == item_id && text.starts_with(&shown) {
                        eprintln!("{}", &text[shown.len()..]);
                    } else {
                        eprintln!("codex> {text}");
                    }
                }
                ThreadItemPayload::CommandExecution { exit_code: Some(code), .. } => eprintln!("[exit {code}]"),
                _ => {}
            },
            ServerNotification::Error(n) => eprintln!("Error: {}", printable(&n.error.message)),
            ServerNotification::Warning(n) => eprintln!("{}", printable(&n.message)),
            ServerNotification::TurnCompleted(n) => {
                if self.streaming.take().is_some() { eprintln!(); }
                match n.turn.status {
                    TurnStatus::Interrupted => eprintln!("Turn interrupted."),
                    TurnStatus::Failed => if let Some(error) = n.turn.error { eprintln!("{}", printable(&error.message)); },
                    _ => {}
                }
                return CodexStatus::InitiateShutdown;
            }
            _ => {}
        }
        CodexStatus::Running
    }
}
