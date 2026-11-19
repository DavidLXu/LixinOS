//! Interactive user-space shell. Parsing and command execution policy stay here.
use super::{keys, print, screen_write, terminal_key};
use crate::abi;
use alloc::{
    collections::{BTreeMap, VecDeque},
    format,
    string::String,
    vec::Vec,
};
use core::{ffi::CStr, slice};
type Result<T> = core::result::Result<T, &'static str>;
#[derive(Debug)]
enum Token {
    Word(String),
    Pipe,
    Write(bool),
    Read,
}
struct Shell {
    cwd: String,
    status: u8,
    vars: BTreeMap<String, String>,
}
fn name_char(ch: char) -> bool {
    ch.is_ascii_alphanumeric() || ch == '_'
}
unsafe extern "C" {
    fn process_call(op: u32, index: u32) -> i32;
}
const MAIL: *mut u8 = 0x443d8000 as *mut u8;
fn request(op: u32, index: u32, arg: &str) -> i32 {
    if arg.len() > 4096 {
        return -1;
    }
    unsafe {
        core::ptr::copy_nonoverlapping(arg.as_ptr(), MAIL, arg.len());
        *MAIL.add(arg.len()) = 0;
        process_call(op, index)
    }
}
fn query(op: u32, index: u32, arg: &str) -> Option<String> {
    let n = usize::try_from(request(op, index, arg)).ok()?;
    if n > 4096 {
        return None;
    }
    Some(String::from(
        core::str::from_utf8(unsafe { slice::from_raw_parts(MAIL, n) }).ok()?,
    ))
}
fn directory<'a>(path: &'a str, prefix: &'a str) -> impl Iterator<Item = (String, u8)> + 'a {
    let mut after = String::new();
    core::iter::from_fn(move || {
        let entry = query(
            abi::READ_DIRECTORY,
            0,
            &format!("{path}\0{prefix}\0{after}"),
        )?;
        let name = String::from(entry.get(1..)?);
        after.clone_from(&name);
        Some((name, entry.as_bytes()[0] - b'0'))
    })
}
impl Shell {
    fn lex(&self, line: &str) -> Result<Vec<Token>> {
        let mut tokens = Vec::new();
        let mut word = String::new();
        let mut started = false;
        let mut quote = None;
        let mut chars = line.chars().peekable();
        while let Some(ch) = chars.next() {
            if quote.is_none() && (ch.is_ascii_whitespace() || "|<>".contains(ch)) {
                if started {
                    tokens.push(Token::Word(core::mem::take(&mut word)));
                    started = false;
                }
                match ch {
                    '|' => tokens.push(Token::Pipe),
                    '<' => tokens.push(Token::Read),
                    '>' => tokens.push(Token::Write(chars.next_if_eq(&'>').is_some())),
                    _ => {}
                }
                continue;
            }
            if quote.is_none() && ch == '#' && !started {
                break;
            }
            started = true;
            if quote == Some(ch) {
                quote = None;
            } else if quote.is_none() && matches!(ch, '\'' | '"') {
                quote = Some(ch);
            } else if ch == '\\' && quote != Some('\'') {
                let next = chars.next().ok_or("unfinished escape")?;
                if quote == Some('"') && !matches!(next, '"' | '$' | '\\' | '`') {
                    word.push('\\');
                }
                word.push(next);
            } else if ch == '$' && quote != Some('\'') {
                if chars.next_if_eq(&'?').is_some() {
                    word.push_str(&format!("{}", self.status));
                    continue;
                }
                let mut name = String::new();
                while chars.peek().is_some_and(|c| name_char(*c)) {
                    name.push(chars.next().unwrap());
                }
                if name.is_empty() {
                    word.push('$');
                } else if name == "PWD" {
                    word.push_str(&self.cwd);
                } else if let Some(value) = self.vars.get(&name) {
                    word.push_str(&value);
                }
            } else if quote.is_none() && ";&".contains(ch) {
                return Err("unsupported operator");
            } else {
                word.push(ch);
            }
            if word.len() > 4096 {
                return Err("word too long");
            }
        }
        if quote.is_some() {
            return Err("unclosed quote");
        }
        if started {
            tokens.push(Token::Word(word));
        }
        Ok(tokens)
    }

    fn prompt(&self) {
        if crate::geometry() >> 32 != 0 {
            print("\x1b[1;36m");
        }
        print("lixin:");
        print(&self.cwd);
        print("$ ");
        if crate::geometry() >> 32 != 0 {
            print("\x1b[0m");
        }
    }

    fn complete(&self, line: &mut String, list_matches: bool) {
        // Mark the cursor before lexing so an empty argument after a space
        // remains distinguishable from a completed word. Try closing an open quote.
        const CURSOR: char = '\u{e000}';
        let Some((mut tokens, quote)) = ["", "'", "\""].into_iter().find_map(|quote| {
            self.lex(&format!("{line}{CURSOR}{quote}"))
                .ok()
                .map(|tokens| (tokens, quote))
        }) else {
            return;
        };
        let Some(Token::Word(word)) = tokens.pop() else {
            return;
        };
        let Some(prefix) = word.strip_suffix(CURSOR) else {
            return;
        };
        let stage = tokens.rsplit(|t| matches!(t, Token::Pipe)).next().unwrap();
        let mut matches = Vec::new();
        let leaf;
        if stage.is_empty() && !prefix.contains('/') {
            leaf = prefix;
            if "shell".starts_with(prefix) {
                matches.push(("shell".into(), false));
            }
            for dir in self
                .vars
                .get("PATH")
                .map(String::as_str)
                .unwrap_or("")
                .split(':')
            {
                for (name, kind) in directory(if dir.is_empty() { "." } else { dir }, prefix) {
                    if kind == 2 && name.starts_with(prefix) {
                        matches.push((name, false));
                    }
                }
            }
            matches.sort_unstable();
            matches.dedup();
        } else {
            let (dir, basename) = match prefix.rsplit_once('/') {
                Some(("", name)) => ("/", name),
                Some(pair) => pair,
                None => (".", prefix),
            };
            leaf = basename;
            let dirs_only = matches!(stage.first(), Some(Token::Word(cmd)) if cmd == "cd")
                && !matches!(stage.last(), Some(Token::Read | Token::Write(_)));
            for (name, kind) in directory(dir, leaf) {
                if !name.starts_with(leaf)
                    || (name.starts_with('.') && !leaf.starts_with('.'))
                    || (dirs_only && kind != 1)
                    || (stage.is_empty() && kind == 0)
                {
                    continue;
                }
                matches.push((name, kind == 1));
            }
        }
        let Some((first, is_dir)) = matches.first() else {
            return;
        };
        let mut common = first.as_str();
        for (name, _) in &matches[1..] {
            let len = common
                .chars()
                .zip(name.chars())
                .take_while(|(a, b)| a == b)
                .map(|(c, _)| c.len_utf8())
                .sum();
            common = &common[..len];
        }
        let mut suffix = String::new();
        for c in common[leaf.len()..].chars() {
            if quote == "'" && c == '\'' {
                suffix.push_str("'\\''");
                continue;
            }
            if (quote.is_empty() && " \\$\"'|<>;&`#".contains(c))
                || (quote == "\"" && "\\$\"`".contains(c))
            {
                suffix.push('\\');
            }
            suffix.push(c);
        }
        if matches.len() == 1 {
            if *is_dir {
                suffix.push('/');
            } else {
                suffix.push_str(quote);
                suffix.push(' ');
            }
        } else if list_matches {
            print("\n");
            for (name, directory) in &matches {
                print(name);
                if *directory {
                    print("/");
                }
                print("  ");
            }
            print("\n");
            self.prompt();
            print(line);
        }
        if line.len() + suffix.len() <= 512 {
            print(&suffix);
            line.push_str(&suffix);
        }
    }
}
struct PromptInput {
    line: String,
    draft: String,
    history: VecDeque<String>,
    position: usize,
    previous_tab: bool,
    skip_lf: bool,
}

impl PromptInput {
    fn new(history: VecDeque<String>) -> Self {
        Self {
            position: history.len(),
            history,
            line: String::new(),
            draft: String::new(),
            previous_tab: false,
            skip_lf: false,
        }
    }
}

#[path = "execute.rs"]
mod execute;
impl Shell {
    fn read_key(&self, input: &mut PromptInput) -> bool {
        let byte = keys::emacs_key(terminal_key());
        if core::mem::take(&mut input.skip_lf) && byte == b'\n' {
            return false;
        }
        let repeated_tab = input.previous_tab && byte == 9;
        input.previous_tab = byte == 9;
        match byte {
            9 => self.complete(&mut input.line, repeated_tab),
            b'\r' | b'\n' => {
                input.skip_lf = byte == b'\r';
                print("\n");
                return true;
            }
            16 | 14 => {
                let next = if byte == 16 {
                    input.position.saturating_sub(1)
                } else {
                    (input.position + 1).min(input.history.len())
                };
                if next == input.position {
                    return false;
                }
                if input.position == input.history.len() {
                    input.draft.clone_from(&input.line);
                }
                input.position = next;
                let selected = input.history.get(input.position).unwrap_or(&input.draft);
                let common = input
                    .line
                    .bytes()
                    .zip(selected.bytes())
                    .take_while(|(old, new)| old == new)
                    .count();
                for _ in common..input.line.len() {
                    print("\x08 \x08");
                }
                print(&selected[common..]);
                input.line.clone_from(selected);
            }
            8 | 127 if input.line.pop().is_some() => print("\x08 \x08"),
            32..=126 if input.line.len() < 512 => {
                input.line.push(char::from(byte));
                unsafe {
                    screen_write(&byte, 1);
                }
            }
            _ => (),
        }
        false
    }
}

struct Session {
    shell: Shell,
    input: PromptInput,
}

impl Session {
    fn new(cwd: String, history: VecDeque<String>) -> Self {
        Self {
            shell: Shell {
                cwd,
                status: 0,
                vars: BTreeMap::from([("PATH".into(), "/bin".into())]),
            },
            input: PromptInput::new(history),
        }
    }

    fn command(&mut self) -> Result<Vec<Token>> {
        let input = &mut self.input;
        let line = core::mem::take(&mut input.line);
        let tokens = self.shell.lex(&line);
        if !tokens.as_ref().is_ok_and(|ts| self.shell.skip_history(ts))
            && input.history.back() != Some(&line)
        {
            if request(abi::HISTORY_APPEND, 0, &line) < 0 {
                print("could not save history\n");
            }
            if input.history.len() >= 64 {
                input.history.pop_front();
            }
            input.history.push_back(line);
        }
        input.position = input.history.len();
        input.draft.clear();
        input.previous_tab = false;
        tokens
    }
}

struct Terminal {
    sessions: BTreeMap<usize, Session>,
    active: usize,
    next: usize,
}
impl Terminal {
    fn new(cwd: String, history: VecDeque<String>) -> Self {
        Self {
            sessions: BTreeMap::from([(1, Session::new(cwd, history))]),
            active: 1,
            next: 2,
        }
    }
    fn restore(&mut self) {
        let shell = &mut self.sessions.get_mut(&self.active).unwrap().shell;
        if query(abi::CHDIR, 0, &shell.cwd).is_none() {
            shell.cwd = query(abi::CHDIR, 0, "/").unwrap();
        }
    }
    fn select(&mut self, id: usize) {
        self.active = id;
        self.restore();
        print(&format!("[shell {id}]\n"));
    }
    fn manage(&mut self, args: &[Token]) -> Result<bool> {
        match args {
            [Token::Word(op)] if op == "new" => {
                let id = self.next;
                self.next = id.checked_add(1).ok_or("shell IDs exhausted")?;
                self.sessions
                    .insert(id, Session::new("/".into(), VecDeque::new()));
                self.select(id);
            }
            [Token::Word(op)] if op == "list" => {
                for (id, s) in &self.sessions {
                    let mark = if *id == self.active { "*" } else { " " };
                    print(&format!("{mark} {id}  {}\n", s.shell.cwd));
                }
            }
            [Token::Word(op), Token::Word(id)] if op == "use" => {
                let id = id
                    .parse()
                    .ok()
                    .filter(|i| self.sessions.contains_key(i))
                    .ok_or("unknown shell ID")?;
                self.select(id);
            }
            [Token::Word(op)] if op == "close" => return Ok(true),
            _ => return Err("usage: shell new | shell list | shell use ID | shell close"),
        }
        Ok(false)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn sh_main(initial: *const core::ffi::c_char, flags: u32) -> i32 {
    let cwd = unsafe { CStr::from_ptr(initial) }.to_str().unwrap().into();
    let history = (0..)
        .map_while(|i| query(abi::HISTORY_GET, i, ""))
        .collect();
    let mut terminals = BTreeMap::from([(1u32, Terminal::new(cwd, history))]);
    let mut current = 1;
    if flags & 2 != 0 && crate::geometry() >> 32 != 0 && crate::geometry() & 65535 >= 60 {
        print("\x1b[36m  L I X I N O S   SYSTEM CONSOLE\x1b[0m\n");
        print(
            "  ARM64 / QEMU virt   Disk mounted\n  help  Commands     htop  System monitor     shutdown  Power off\n\n",
        );
    }
    if flags & 2 != 0 { print("LixinOS sh (Rust shell)\n"); }
    terminals[&1].sessions[&1].shell.prompt();
    if flags & 1 != 0 {
        request(abi::PANE_READY, 0, "");
    }
    let remote = flags & 4 != 0;
    loop {
        if remote {
            let event = request(abi::NEXT_TERMINAL, 0, "") as u32;
            let id = event & 0x3fffffff;
            if event & (1 << 30) != 0 {
                if let alloc::collections::btree_map::Entry::Vacant(slot) = terminals.entry(id) {
                    let t = slot.insert(Terminal::new("/".into(), VecDeque::new()));
                    print("LixinOS sh (Rust shell)\n");
                    t.sessions[&1].shell.prompt();
                }
            }
            if id != current {
                if let Some(t) = terminals.get_mut(&id) {
                    t.restore();
                }
                current = id;
            }
            if event & (1 << 30) != 0 {
                continue;
            }
        }
        let Some(t) = terminals.get_mut(&current) else {
            crate::read_byte();
            continue;
        };
        let s = t.sessions.get_mut(&t.active).unwrap();
        if !s.shell.read_key(&mut s.input) {
            continue;
        }
        let result = match s.command() {
            Ok(ts) if matches!(ts.first(),Some(Token::Word(cmd)) if cmd == "shell") => {
                s.shell.status = 0;
                t.manage(&ts[1..]).map_err(String::from)
            }
            Ok(ts) => s.shell.execute(&ts),
            Err(e) => Err(e.into()),
        };
        match result {
            Ok(true) => {
                let closed = t.sessions.remove(&t.active).unwrap();
                if let Some(&id) = t.sessions.keys().next() {
                    t.select(id);
                } else if remote {
                    terminals.remove(&current);
                    request(abi::CLOSE_TERMINAL, 0, "");
                    continue;
                } else {
                    return closed.shell.status.into();
                }
            }
            Err(e) => {
                t.sessions.get_mut(&t.active).unwrap().shell.status = 1;
                print(&format!("shell: {e}\n"));
            }
            Ok(false) => (),
        }
        t.sessions[&t.active].shell.prompt();
    }
}
