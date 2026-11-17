use crate::{keyboard::read_key, print, put};
use alloc::{
    collections::{BTreeMap, VecDeque},
    format,
    string::String,
    vec::Vec,
};

use alloc::rc::Rc;
use core::{cell::RefCell, fmt::Write};

type Result<T> = core::result::Result<T, &'static str>;
use crate::disk::{self, ENTRY_LIMIT, FILE_LIMIT, HISTORY_LIMIT};
const COMMANDS: &[&str] = &[
    "agent", "cat", "cd", "cecho", "clear", "echo", "grep", "help", "htop", "ls", "mkdir", "nano",
    "pwd", "python", "rm", "shutdown", "tcc", "touch", "wc",
];

#[derive(Debug)]
enum Token {
    Word(String),
    Pipe,
    Write(bool),
    Read,
}

struct Volume {
    disk: crate::disk::Disk,
    files: crate::disk::Files,
}

struct Shell {
    agent_bridge: Rc<RefCell<Option<crate::agent::Bridge>>>,
    volume: Rc<RefCell<Volume>>,
    started: u64,
    env: Environment,
}

struct Environment {
    cwd: String,
    vars: BTreeMap<String, String>,
}

impl Default for Environment {
    fn default() -> Self {
        Self {
            cwd: "/".into(),
            vars: BTreeMap::new(),
        }
    }
}

fn name_char(ch: char) -> bool {
    ch.is_ascii_alphanumeric() || ch == '_'
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
                let mut name = String::new();
                while chars.peek().is_some_and(|c| name_char(*c)) {
                    name.push(chars.next().unwrap());
                }
                if name.is_empty() {
                    word.push('$');
                } else if name == "PWD" {
                    word.push_str(&self.env.cwd);
                } else if let Some(value) = self.env.vars.get(&name) {
                    word.push_str(value);
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
        if cfg!(feature = "window-console") {
            print("\x1b[1;36m");
        }
        print("lixin:");
        print(&self.env.cwd);
        print("$ ");
        if cfg!(feature = "window-console") {
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
        if stage.is_empty() {
            leaf = prefix;
            matches.extend(
                COMMANDS
                    .iter()
                    .filter(|s| s.starts_with(prefix))
                    .map(|s| ((*s).into(), false)),
            );
        } else {
            let (dir, basename) = match prefix.rsplit_once('/') {
                Some(("", name)) => ("/", name),
                Some(pair) => pair,
                None => (".", prefix),
            };
            leaf = basename;
            let Ok(parent) = self.path(dir) else { return };
            let base = format!("{}/", parent.trim_end_matches('/'));
            let dirs_only = matches!(stage.first(), Some(Token::Word(cmd)) if cmd == "cd")
                && !matches!(stage.last(), Some(Token::Read | Token::Write(_)));
            for (path, value) in self.volume.borrow().files.range(base.clone()..) {
                let Some(name) = path.strip_prefix(&base) else {
                    break;
                };
                if name.is_empty()
                    || name.contains('/')
                    || !name.starts_with(leaf)
                    || (name.starts_with('.') && !leaf.starts_with('.'))
                    || (dirs_only && value.is_some())
                {
                    continue;
                }
                matches.push((String::from(name), value.is_none()));
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

    fn path(&self, name: &str) -> Result<String> {
        if name.is_empty() {
            return Err("empty path");
        }
        let mut path = if name.starts_with('/') {
            String::new()
        } else {
            self.env.cwd.clone()
        };
        for part in name.split('/').filter(|p| !p.is_empty()) {
            if !path.is_empty() && self.volume.borrow().files.get(&path) != Some(&None) {
                return Err("not a directory");
            }
            match part {
                "." => (),
                ".." => path.truncate(path.rfind('/').unwrap_or(0)),
                _ => {
                    if path != "/" {
                        path.push('/');
                    }
                    path.push_str(part);
                }
            }
        }
        if path.is_empty() {
            path.push('/');
        }
        if path.len() > 512 {
            return Err("path too long");
        }
        Ok(path)
    }

    fn store(&mut self, path: String, value: Option<String>) -> Result<()> {
        self.store_bytes(path, value.map(String::into_bytes))
    }
    fn store_bytes(&mut self, path: String, value: Option<Vec<u8>>) -> Result<()> {
        let Volume { disk, files } = &mut *self.volume.borrow_mut();
        if !files.contains_key(&path) && files.len() >= ENTRY_LIMIT {
            return Err("filesystem full");
        }
        let old = files.insert(path.clone(), value);
        if let Err(err) = disk.save(files) {
            match old {
                Some(value) => {
                    files.insert(path, value);
                }
                None => {
                    files.remove(&path);
                }
            }
            return Err(err);
        }
        Ok(())
    }

    fn file(&self, name: &str) -> Result<String> {
        String::from_utf8(self.bytes(name)?).map_err(|_| "binary file")
    }

    fn with_agent<T>(&self, action: impl FnOnce(&mut crate::agent::Bridge) -> Result<T>) -> Result<T> {
        let mut bridge = self.agent_bridge.try_borrow_mut().map_err(|_| "agent is busy")?;
        if bridge.is_none() {
            *bridge = Some(crate::agent::Bridge::connect()?);
        }
        action(bridge.as_mut().unwrap())
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

    fn list(&self, args: &[String]) -> Result<String> {
        let (mut all, mut long, mut human) = (false, false, false);
        let mut options = true;
        let mut operand = None;
        for arg in args {
            if options && arg == "--" {
                options = false;
            } else if options && arg.starts_with('-') && arg.len() > 1 {
                for flag in arg[1..].chars() {
                    match flag {
                        'a' => all = true,
                        'l' => long = true,
                        'h' => human = true,
                        _ => return Err("usage: ls [-lah] [path]"),
                    }
                }
            } else if operand.replace(arg.as_str()).is_some() {
                return Err("usage: ls [-lah] [path]");
            }
        }

        let name = operand.unwrap_or(".");
        let path = self.path(name)?;
        let volume = self.volume.borrow();
        let entry = volume.files.get(&path).ok_or("path not found")?;
        let mut out = String::new();
        let mut row = |name: &str, size: Option<usize>| {
            if long {
                let (kind, size) = match size {
                    None => ('d', String::from("-")),
                    Some(n) if human && n >= 1024 => {
                        let tenths = (n * 10 + 512) / 1024;
                        ('-', format!("{}.{} KiB", tenths / 10, tenths % 10))
                    }
                    Some(n) if human => ('-', format!("{n} B")),
                    Some(n) => ('-', format!("{n}")),
                };
                write!(out, "{kind} {size:>8}  ").unwrap();
            }
            writeln!(out, "{name}").unwrap();
        };
        if let Some(contents) = entry {
            row(name, Some(contents.len()));
        } else {
            if all {
                row(".", None);
                row("..", None);
            }
            for (name, value) in disk::children(&volume.files, &path) {
                if all || !name.starts_with('.') {
                    row(name, value.as_ref().map(Vec::len));
                }
            }
        }
        Ok(out)
    }

    fn command(&mut self, args: &[String], input: String) -> Result<String> {
        let (cmd, args) = args.split_first().ok_or("missing command")?;
        let arg = || {
            if args.len() == 1 {
                Ok(args[0].as_str())
            } else {
                Err("expected one argument")
            }
        };
        match cmd.as_str() {
            "help" => Ok(format!("{}\n", COMMANDS.join(" "))),
            "echo" => Ok(format!("{}\n", args.join(" "))),
            "pwd" if args.is_empty() => Ok(format!("{}\n", self.env.cwd)),
            "cat" if args.is_empty() => Ok(input),
            "cat" => Ok(self.file(arg()?)?.into()),
            "grep" => {
                let pattern = arg()?;
                Ok(input
                    .lines()
                    .filter(|line| line.contains(pattern))
                    .map(|line| format!("{line}\n"))
                    .collect())
            }
            "wc" if arg()? == "-l" => Ok(format!(
                "{}\n",
                input.bytes().filter(|&b| b == b'\n').count()
            )),
            "cd" => {
                let path = self.path(if args.is_empty() { "/" } else { arg()? })?;
                if self.volume.borrow().files.get(&path) != Some(&None) {
                    return Err("directory not found");
                }
                self.env.cwd = path;
                Ok(String::new())
            }
            "ls" => self.list(args),
            "mkdir" | "touch" | "rm" => {
                let path = self.path(arg()?)?;
                match cmd.as_str() {
                    "mkdir" if self.volume.borrow().files.contains_key(&path) => {
                        return Err("already exists");
                    }
                    "mkdir" => self.store(path, None)?,
                    "touch" if !self.volume.borrow().files.contains_key(&path) => {
                        self.store(path, Some(String::new()))?
                    }
                    "rm" => {
                        self.bytes(arg()?)?;
                        let Volume { disk, files } = &mut *self.volume.borrow_mut();
                        let old = files.remove(&path).unwrap();
                        if let Err(err) = disk.save(files) {
                            files.insert(path, old);
                            return Err(err);
                        }
                    }
                    _ => (),
                }
                Ok(String::new())
            }
            _ if args.is_empty() && cmd.contains('=') => {
                let (name, value) = cmd.split_once('=').unwrap();
                if name.is_empty()
                    || name.starts_with(|c: char| c.is_ascii_digit())
                    || !name.chars().all(name_char)
                    || name == "PWD"
                {
                    return Err("invalid variable name");
                }
                if self.env.vars.len() >= 64 && !self.env.vars.contains_key(name) {
                    return Err("too many variables");
                }
                self.env.vars.insert(name.into(), value.into());
                Ok(String::new())
            }
            _ => Err("unknown command or unsupported arguments"),
        }
    }

    fn execute(&mut self, line: &str, tokens: Result<Vec<Token>>) -> Result<()> {
        if line.trim() == "shutdown" {
            print("Shutting down LixinOS...\n");
            // QEMU virt exposes PSCI SYSTEM_OFF via HVC at EL1.
            unsafe {
                core::arch::asm!("hvc #0", inout("x0") 0x8400_0008_u64 => _, clobber_abi("C"))
            };
            return Err("power off failed");
        }
        if line.trim() == "clear" {
            print("\x1b[2J\x1b[H");
            return Ok(());
        }
        if line.trim() == "cecho" {
            return crate::user::run(include_bytes!("../target/user-build/echo.bin"), None);
        }
        if line.trim() == "htop" {
            crate::monitor::show(self.started, || {
                disk::file_counts(&self.volume.borrow().files)
            });
            return Ok(());
        }
        let tokens = tokens?;
        if tokens.is_empty() {
            return Ok(());
        }
        if matches!(tokens.first(), Some(Token::Word(cmd)) if cmd == "agent") {
            if matches!(tokens.get(1), Some(Token::Word(mode)) if mode == "inspect") {
                let [_, _, Token::Word(question)] = &tokens[..] else {
                    return Err("usage: agent inspect \"question\"");
                };
                if question.trim().is_empty() {
                    return Err("usage: agent inspect \"question\"");
                }
                return self.with_agent(|bridge| {
                    crate::agent::inspect(bridge, question, &self.env.cwd, |tool, path| {
                        self.inspect_query(tool, path)
                    })
                });
            }
            let [_, Token::Word(name), Token::Word(task)] = &tokens[..] else {
                return Err("usage: agent inspect \"question\" or agent output.py \"task\"");
            };
            if task.trim().is_empty() || !name.ends_with(".py") {
                return Err("usage: agent output.py \"task\"");
            }
            let path = self.path(name)?;
            if self.volume.borrow().files.contains_key(&path) {
                return Err("agent output already exists");
            }
            let script = self.with_agent(|bridge| crate::agent::generate(bridge, &path, task, FILE_LIMIT))?;
            if self.volume.borrow().files.contains_key(&path) {
                return Err("agent output already exists");
            }
            self.store(path.clone(), Some(script))?;
            print(&format!("agent: saved {path}\n"));
            return Ok(());
        }
        if matches!(tokens.first(), Some(Token::Word(cmd)) if cmd == "nano") {
            let [_, Token::Word(name)] = &tokens[..] else {
                return Err("usage: nano filename");
            };
            let path = self.path(name)?;
            let mut original = match self.volume.borrow().files.get(&path) {
                Some(Some(bytes)) => {
                    Some(String::from_utf8(bytes.clone()).map_err(|_| "binary file")?)
                }
                Some(None) => return Err("is a directory"),
                None => None,
            };
            let text = original.clone().unwrap_or_default();
            crate::editor::edit(&path, &text, FILE_LIMIT, |contents| {
                let expected = original.as_ref().map(|s| Some(s.as_bytes()));
                let current = self.volume.borrow();
                if current.files.get(&path).map(Option::as_deref) != expected {
                    return Err("file changed in other pane");
                }
                drop(current);
                self.store(path.clone(), Some(contents.clone()))?;
                original = Some(contents);
                Ok(())
            });
            return Ok(());
        }
        if matches!(tokens.first(), Some(Token::Word(cmd)) if cmd == "tcc") {
            match &tokens[..] {
                [_, Token::Word(flag), Token::Word(path)] if flag == "-run" => {
                    return crate::user::run(crate::user::TCC, Some((path, &self.file(path)?)));
                }
                [_, Token::Word(src), Token::Word(flag), Token::Word(dst)] if flag == "-o" => {
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
        if let [Token::Word(path)] = &tokens[..] {
            if path.contains('/') {
                return crate::user::run_saved(&self.bytes(path)?);
            }
        }
        if matches!(tokens.first(), Some(Token::Word(cmd)) if cmd == "python") {
            let script = match &tokens[..] {
                [_] => None,
                [_, Token::Word(path)] => Some((path.as_str(), self.file(path)?)),
                _ => return Err("usage: python [script.py]"),
            };
            return crate::user::run(
                crate::user::PYTHON,
                script.as_ref().map(|(path, text)| (*path, text.as_str())),
            );
        }
        let mut input = String::new();
        for stage in tokens.split(|t| matches!(t, Token::Pipe)) {
            let mut args = Vec::new();
            let mut redirect = None;
            let mut iter = stage.iter();
            while let Some(token) = iter.next() {
                match token {
                    Token::Word(w) => args.push(w.clone()),
                    Token::Read | Token::Write(_) => {
                        let Some(Token::Word(name)) = iter.next() else {
                            return Err("expected filename");
                        };
                        if let Token::Write(append) = token {
                            let path = self.path(name)?;
                            if self.volume.borrow().files.get(&path) == Some(&None) {
                                return Err("is a directory");
                            }
                            redirect = Some((path, *append));
                        } else {
                            input = self.file(name)?.into();
                        }
                    }
                    Token::Pipe => unreachable!(),
                }
            }
            input = self.command(&args, input)?;
            if let Some((path, append)) = redirect {
                let volume = self.volume.borrow();
                let old = if append {
                    volume
                        .files
                        .get(&path)
                        .and_then(Option::as_deref)
                        .map(core::str::from_utf8)
                        .transpose()
                        .map_err(|_| "binary file")?
                        .unwrap_or("")
                } else {
                    ""
                };
                if old.len() + input.len() > FILE_LIMIT {
                    return Err("file too large");
                }
                let contents = format!("{old}{input}");
                drop(volume);
                self.store(path, Some(contents))?;
                input.clear();
            }
        }
        print(&input);
        Ok(())
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
    fn reset(&mut self) {
        self.line.clear();
        self.draft.clear();
        self.previous_tab = false;
        self.position = self.history.len();
    }
}
pub fn run() -> ! {
    let started = crate::monitor::ticks();
    let (mut disk, files) = crate::disk::Disk::mount().unwrap_or_else(|err| {
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
    let shell = Shell {
        agent_bridge: Rc::new(RefCell::new(None)),
        started,
        volume: Rc::new(RefCell::new(Volume { disk, files })),
        env: Environment::default(),
    };
    #[cfg(feature = "split")]
    {
        let right = Shell {
            started,
            agent_bridge: shell.agent_bridge.clone(),
            volume: shell.volume.clone(),
            env: Environment::default(),
        };
        let left_history = history.clone();
        crate::panes::init(alloc::boxed::Box::new(move || session(right, history)));
        session(shell, left_history)
    }
    #[cfg(not(feature = "split"))]
    session(shell, history)
}

fn session(mut shell: Shell, history: VecDeque<String>) -> ! {
    let mut input = PromptInput::new(history);
    if cfg!(feature = "window-console") && !cfg!(feature = "split") {
        print(concat!(
            "\x1b[2J\x1b[H\n",
            "\x1b[36m  +--------------------------------------------------------+\n",
            "  |\x1b[1m  L I X I N O S                         SYSTEM CONSOLE  \x1b[0;36m|\n",
            "  +--------------------------------------------------------+\x1b[0m\n",
            "\n",
            "  ARM64  /  QEMU virt     \x1b[32mDisk mounted\x1b[0m\n",
            "  help  Commands          htop  System monitor     shutdown  Power off\n",
            "\n",
        ));
    }
    shell.prompt();
    #[cfg(feature = "split")]
    crate::panes::switch(); // Start both panes at a prompt, with the left focused.
    loop {
        loop {
            let byte = read_key();
            if core::mem::take(&mut input.skip_lf) && byte == b'\n' {
                continue;
            }
            let repeated_tab = input.previous_tab && byte == 9;
            input.previous_tab = byte == 9;
            match byte {
                9 => shell.complete(&mut input.line, repeated_tab),
                b'\r' | b'\n' => {
                    input.skip_lf = byte == b'\r';
                    print("\n");
                    break;
                }
                16 | 14 => {
                    let next = if byte == 16 {
                        input.position.saturating_sub(1)
                    } else {
                        (input.position + 1).min(input.history.len())
                    };
                    if next == input.position {
                        continue;
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
                    put(byte);
                }
                _ => (),
            }
        }
        // Inspection stays read-only, including the persistent command history.
        let tokens = shell.lex(&input.line);
        let inspecting = tokens.as_deref().is_ok_and(|tokens| {
            matches!(tokens, [Token::Word(cmd), Token::Word(mode), ..]
                if cmd == "agent" && mode == "inspect")
        });
        if !inspecting && !matches!(input.line.trim(), "" | "shutdown")
            && input.history.back() != Some(&input.line)
        {
            if let Err(err) = shell.volume.borrow_mut().disk.append_history(&input.line) {
                print(err);
                print("\n");
            }
            if input.history.len() == HISTORY_LIMIT {
                input.history.pop_front();
            }
            input.history.push_back(input.line.clone());
        }
        if let Err(error) = shell.execute(&input.line, tokens) {
            print("shell: ");
            print(error);
            print("\n");
        }
        input.reset();
        shell.prompt();
    }
}
