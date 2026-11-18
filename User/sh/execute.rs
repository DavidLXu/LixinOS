use super::{Shell, Token, query, request};
use crate::abi;
use alloc::{format, string::String, vec::Vec};
type Result<T> = core::result::Result<T, String>;
unsafe extern "C" {
    fn wait_child();
}

fn call(op: u32, flags: u32, arg: &str) -> Result<i32> {
    let result = request(op, flags, arg);
    if result < 0 {
        Err(query(abi::LAST_ERROR, 0, "").unwrap_or_else(|| "process request failed".into()))
    } else {
        Ok(result)
    }
}
struct Stage<'a> {
    args: Vec<&'a str>,
    input: Option<&'a str>,
    output: Option<(&'a str, bool)>,
}
impl<'a> Stage<'a> {
    fn parse(tokens: &'a [Token]) -> Result<Self> {
        let mut s = Self {
            args: Vec::new(),
            input: None,
            output: None,
        };
        let mut it = tokens.iter();
        while let Some(t) = it.next() {
            match t {
                Token::Word(w) => s.args.push(w),
                Token::Read | Token::Write(_) => {
                    let Some(Token::Word(file)) = it.next() else {
                        return Err("expected filename".into());
                    };
                    match t {
                        Token::Write(append) => s.output = Some((file, *append)),
                        _ => s.input = Some(file),
                    }
                }
                Token::Pipe => unreachable!(),
            }
        }
        if s.args.is_empty() {
            return Err("missing command".into());
        }
        Ok(s)
    }
}
impl Shell {
    // The executable path and optional legacy launcher identity come from stat.
    fn program(&self, word: &str) -> Result<(String, String)> {
        let name = if word == "q" { "shutdown" } else { word };
        let lookup = |path: String| {
            query(abi::STAT, 0, &path)
                .filter(|k| k == "x" || k.starts_with('@'))
                .map(|kind| (path, kind))
        };
        if name.contains('/') {
            return lookup(name.into()).ok_or_else(|| {
                match query(abi::STAT, 0, name).as_deref() {
                    Some("d") => "is a directory",
                    Some(_) => "invalid LixinOS executable",
                    None => "file not found",
                }
                .into()
            });
        }
        self.vars
            .get("PATH")
            .into_iter()
            .flat_map(|p| p.split(':'))
            .find_map(|dir| lookup(format!("{}/{name}", if dir.is_empty() { "." } else { dir })))
            .ok_or_else(|| "unknown command".into())
    }
    pub(super) fn skip_history(&self, ts: &[Token]) -> bool {
        let Some(Token::Word(name)) = ts.first() else {
            return ts.is_empty();
        };
        if ts.len() != 1 && !matches!(ts.get(1),Some(Token::Word(m)) if m=="inspect") {
            return false;
        }
        self.program(name).is_ok_and(|(_, k)| {
            (ts.len() == 1 && k == "@shutdown")
                || (k == "@agent" && matches!(ts.get(1),Some(Token::Word(m)) if m=="inspect"))
        })
    }
    pub(super) fn execute(&mut self, ts: &[Token]) -> Result<bool> {
        if ts.is_empty() {
            return Ok(false);
        }
        let stages = ts
            .split(|t| matches!(t, Token::Pipe))
            .map(Stage::parse)
            .collect::<Result<Vec<_>>>()?;
        call(abi::CLEAR_STREAM, 0, "")?;
        for (i, s) in stages.iter().enumerate() {
            let name = s.args[0];
            let capture = i + 1 < stages.len() || s.output.is_some();
            let stream = i != 0 || s.input.is_some();
            if let Some(input) = s.input {
                call(abi::READ_STREAM, 0, input)?;
            }
            if let Some((dest, _)) = s.output {
                if query(abi::STAT, 0, dest).as_deref() == Some("d") {
                    return Err("is a directory".into());
                }
            }
            if name == "exit" && s.args.len() == 1 && stages.len() == 1 {
                return Ok(true);
            }
            self.status = 0;
            if let Some((key, val)) = name.split_once('=').filter(|(k, _)| !k.contains('/')) {
                if s.args.len() != 1
                    || key.is_empty()
                    || key.as_bytes()[0].is_ascii_digit()
                    || !key.chars().all(super::name_char)
                    || key == "PWD"
                {
                    return Err("invalid variable name".into());
                }
                if self.vars.len() >= 64 && !self.vars.contains_key(key) {
                    return Err("too many variables".into());
                }
                self.vars.insert(key.into(), val.into());
                call(abi::CLEAR_STREAM, 0, "")?;
            } else {
                let (path, kind) = self.program(name)?;
                match kind.as_str() {
                    "@cd" => {
                        if s.args.len() > 2 {
                            return Err("expected one argument".into());
                        }
                        self.cwd = query(abi::CHDIR, 0, s.args.get(1).copied().unwrap_or("/"))
                            .ok_or("directory not found")?;
                        call(abi::CLEAR_STREAM, 0, "")?;
                    }
                    "@help" => {
                        let text = "agent cat cd cecho clear echo exit git grep help htop kilo ls lust mkdir nano paint pwd python q rm sh shell shutdown tcc touch wc\n";
                        call(abi::CLEAR_STREAM, 0, "")?;
                        if capture {
                            call(abi::APPEND_STREAM, 0, text)?;
                        } else {
                            super::print(text);
                        }
                    }
                    _ => {
                        if (capture || stream)
                            && kind.starts_with('@')
                            && !matches!(kind.as_str(), "@touch" | "@mkdir" | "@rm")
                        {
                            return Err("this command requires a foreground terminal".into());
                        }
                        call(
                            abi::SPAWN_BEGIN,
                            (stream as u32) | ((capture as u32) << 1),
                            &path,
                        )?;
                        for arg in &s.args {
                            call(abi::SPAWN_ARG, 0, arg)?;
                        }
                        unsafe {
                            wait_child();
                        }
                        self.status = call(abi::CHILD_STATUS, 0, "")? as u8;
                        if !capture && self.status != 0 && kind != "@tcc" {
                            super::print(&format!("[exit {}]\n", self.status));
                        }
                    }
                }
            }
            if let Some((dest, append)) = s.output {
                call(abi::STORE_STREAM, append as u32, dest)?;
            }
        }
        Ok(false)
    }
}
