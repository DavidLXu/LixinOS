use super::{COMMAND_COLOR, print_colored, visible};
use crate::{monitor, print};
use alloc::{format, string::String};

pub(super) const PROMPT: &str = "agent> ";
pub(super) const PROMPT_COLOR: &str = "\x1b[96m";
pub(super) const POSITIVE: &str = "\x1b[92m";
pub(super) const ATTENTION: &str = "\x1b[93m";
pub(super) const ERROR: &str = "\x1b[91m";
const MUTED: &str = "\x1b[90m";
const PANEL: &str = "\x1b[37;40m";
const TITLE: &str = "\x1b[30;106m";
const REPLY: &str = "\x1b[95m";

pub(super) fn columns() -> usize {
    #[cfg(feature = "retina")]
    { crate::graphics::columns() }
    #[cfg(not(feature = "retina"))]
    { 80 }
}

fn fit(text: &str, width: usize) -> String {
    if text.chars().count() <= width { return text.into(); }
    let tail: String = text.chars().rev().take(width.saturating_sub(3)).collect();
    format!("...{}", tail.chars().rev().collect::<String>())
}

pub(super) struct Header {
    #[cfg(feature = "retina")]
    _pin: Option<crate::graphics::PinnedBar>,
}

pub(super) fn welcome(root: &str, budget: usize) -> Header {
    #[cfg(feature = "retina")]
    let _pin = crate::graphics::pin_bar(crate::graphics::TopBar {
        title: " LixinOS Agent ",
        short_title: " Agent ",
        context: root.chars().map(|c| if c.is_ascii_graphic() || c == ' ' { c } else { '?' }).collect(),
        badge: " Bypass on ",
        badge_background: 0xffdf9e,
        status: "Ready",
        detail: format!("{budget} tools/turn"),
    });
    #[cfg(feature = "retina")]
    let pinned = _pin.is_some();
    #[cfg(not(feature = "retina"))]
    let pinned = false;
    if !pinned { inline_header(root, budget); }
    print_colored(MUTED, if columns() >= 35 {
        " /help   /bypass   /export   /exit"
    } else {
        " /help   /bypass"
    });
    print("\n Bypass auto-approves commands that can change guest files.\n\n");
    Header {
        #[cfg(feature = "retina")]
        _pin,
    }
}

fn inline_header(root: &str, budget: usize) {
    let width = columns().saturating_sub(1);
    let path = visible(root, 512);
    print("\n");
    let title = " LixinOS Agent ";
    let mode = " Bypass on ";
    if width < title.len() + mode.len() + 2 {
        print_colored(TITLE, if width >= title.len() { title } else { " Agent " });
        print("\n");
        print_colored(COMMAND_COLOR, &fit(&path, width));
        print("\n");
        print_colored(ATTENTION, "Bypass on");
        print("\n");
        return;
    }
    print_colored(TITLE, title);
    let spacing = width - title.len() - mode.len();
    print_colored(PANEL, &format!("{:spacing$}", ""));
    print_colored("\x1b[30;103m", mode);
    print("\n");
    let budget = format!("{budget} tools/turn");
    let path = fit(&path, width - budget.len() - 3);
    let padding = width - path.chars().count() - budget.len() - 2;
    print_colored(PANEL, &format!(" {path}{:padding$}{budget} ", ""));
    print("\n");
}

pub(super) fn activity(status: &'static str, detail: Option<(usize, usize)>) -> bool {
    #[cfg(feature = "retina")]
    { crate::graphics::update_bar(|bar| {
        bar.status = status;
        if let Some((step, budget)) = detail {
            bar.detail = if status == "Ready" {
                format!("{budget} tools/turn")
            } else {
                format!("{step:02}/{budget} tools")
            };
        }
    }) }
    #[cfg(not(feature = "retina"))]
    { let _ = (status, detail); false }
}

pub(super) fn prompt() {
    print_colored(PROMPT_COLOR, PROMPT);
}

pub(super) fn detail(label: &str, text: &str, color: &str) {
    print("  ");
    print_colored(color, label);
    let spacing = 8usize.saturating_sub(label.chars().count()) + 2;
    print(&format!("{:spacing$}", ""));
    print(text);
    print("\n");
}

pub(super) fn help() {
    print("\n");
    for (key, text) in [
        ("/bypass", "Toggle command auto-approval. Terminal handoff and staged saves ask for approval."),
        ("/export", "Save the conversation in this project."),
        ("/prompt", "Show the system prompt."),
        ("/exit", "Return to the shell."),
        ("Up/Down", "Recall inputs. Down past the newest input restores your draft."),
        ("Ctrl+P", "Pause for a follow-up while waiting for the model or a test."),
        ("Ctrl+G", "Cancel the current turn."),
    ] {
        detail(key, text, COMMAND_COLOR);
    }
    print("\n");
}

pub(super) fn mode(auto_approve: bool) {
    let (label, text, color) = if auto_approve {
        ("Bypass on", "Commands can change guest files.", ATTENTION)
    } else {
        ("Bypass off", "Commands outside the read-only allowlist ask for approval.", POSITIVE)
    };
    #[cfg(feature = "retina")]
    crate::graphics::update_bar(|bar| {
        bar.badge = if auto_approve { " Bypass on " } else { " Bypass off " };
        bar.badge_background = if auto_approve { 0xffdf9e } else { 0xbde5ba };
    });
    detail(label, text, color);
}

pub(crate) fn tool(step: usize, budget: usize, body: &str) {
    print_colored(TITLE, &format!(" [{step:02}/{budget}] "));
    print("  ");
    print_colored(COMMAND_COLOR, body);
    print("\n");
}

pub(super) fn reply(text: &str) {
    print("\n");
    print_colored("\x1b[30;105m", " Agent ");
    print("  ");
    print_colored(REPLY, text);
    print("\n");
}

pub(super) struct ModelWait {
    next: u64,
    step: usize,
    visible: bool,
}

impl ModelWait {
    pub(super) fn new() -> Self {
        Self { next: monitor::ticks() + monitor::seconds_to_ticks(1) / 4, step: 0, visible: false }
    }

    pub(super) fn tick(&mut self, now: u64) {
        if now < self.next { return; }
        self.next = now + monitor::seconds_to_ticks(1) / 8;
        let frame = ["-", "\\", "|", "/"][self.step % 4];
        self.step += 1;
        self.visible = true;
        print("\r\x1b[2K");
        print_colored(COMMAND_COLOR, frame);
        print_colored(MUTED, "  Thinking");
    }

    pub(super) fn clear(self) {
        if self.visible { print("\r\x1b[2K"); }
    }
}

