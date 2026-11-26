//! Guest implementations of literal read-only commands. No shell or external
//! executable interprets any part of the command. Called after Codex approval.
use super::*;
use std::fs;
use std::io::{self, Read};
use std::path::{Component, Path, PathBuf};

const LIMIT: usize = 64 * 1024;

fn denied() -> io::Error {
    io::Error::new(
        io::ErrorKind::PermissionDenied,
        "outside the readable guest workspace",
    )
}

pub(super) fn can_handle(command: &str) -> bool {
    if command.is_empty()
        || !command
            .chars()
            .all(|c| c.is_alphanumeric() || " ./_-".contains(c))
    {
        return false;
    }
    let words: Vec<_> = command.split_ascii_whitespace().collect();
    match words.first().copied() {
        Some("pwd") => words.len() == 1,
        Some("ls") => {
            let mut idx = 1;
            while idx < words.len() && words[idx].starts_with('-') {
                let flags = &words[idx][1..];
                if flags.is_empty() || !flags.bytes().all(|b| matches!(b, b'1' | b'a' | b'l')) {
                    return false;
                }
                idx += 1;
            }
            words.len() - idx <= 1
        }
        Some("cat") => {
            let mut idx = 1;
            while idx < words.len() && words[idx].starts_with('-') {
                if words[idx] != "-n" {
                    return false;
                }
                idx += 1;
            }
            idx < words.len() && !words[idx..].iter().any(|w| w.starts_with('-'))
        }
        Some("head" | "tail") => parse_line_count(&words[1..]).is_some(),
        Some("wc") => parse_wc_args(&words[1..]).is_some(),
        _ => false,
    }
}

fn parse_line_count<'a>(args: &'a [&'a str]) -> Option<(usize, &'a [&'a str])> {
    let mut lines = 10usize;
    let mut idx = 0;
    if let Some(first) = args.first()
        && let Some(rest) = first.strip_prefix('-')
    {
        if rest == "n" {
            let val = args.get(1)?.parse::<usize>().ok()?;
            lines = val;
            idx = 2;
        } else if let Some(val_str) = rest.strip_prefix('n') {
            lines = val_str.parse::<usize>().ok()?;
            idx = 1;
        } else if !rest.is_empty() && rest.bytes().all(|b| b.is_ascii_digit()) {
            lines = rest.parse::<usize>().ok()?;
            idx = 1;
        } else {
            return None;
        }
    }
    let files = &args[idx..];
    if files.is_empty() || files.iter().any(|w| w.starts_with('-')) {
        return None;
    }
    Some((lines, files))
}

fn parse_wc_args<'a>(args: &'a [&'a str]) -> Option<((bool, bool, bool), &'a [&'a str])> {
    let mut show_l = false;
    let mut show_w = false;
    let mut show_c = false;
    let mut idx = 0;
    while idx < args.len() && args[idx].starts_with('-') {
        let flags = &args[idx][1..];
        if flags.is_empty() {
            return None;
        }
        for b in flags.bytes() {
            match b {
                b'l' => show_l = true,
                b'w' => show_w = true,
                b'c' => show_c = true,
                _ => return None,
            }
        }
        idx += 1;
    }
    let files = &args[idx..];
    if files.is_empty() || files.iter().any(|w| w.starts_with('-')) {
        return None;
    }
    if !show_l && !show_w && !show_c {
        show_l = true;
        show_w = true;
        show_c = true;
    }
    Some(((show_l, show_w, show_c), files))
}

fn read_bounded(path: &Path, limit: usize) -> io::Result<Vec<u8>> {
    let file = fs::File::open(path)?;
    if !file.metadata()?.is_file() {
        return Err(io::Error::other("requires a regular file"));
    }
    let mut data = Vec::new();
    file.take((limit + 1) as u64).read_to_end(&mut data)?;
    if data.len() > limit {
        return Err(io::Error::other("file exceeds 64 KiB"));
    }
    Ok(data)
}

pub(super) fn execute(
    req: &UnifiedExecRequest<'_>,
    attempt: &SandboxAttempt<'_>,
) -> io::Result<Vec<u8>> {
    let prepared = req.prepared;
    let root = attempt.sandbox_cwd.to_abs_path().map_err(|_| denied())?;
    let cwd = prepared.cwd.to_abs_path().map_err(|_| denied())?;
    let root = root.as_path();
    let cwd = cwd.as_path();
    let policy = attempt.permissions.file_system_sandbox_policy();
    let check = |path: &Path| -> io::Result<()> {
        if !path.starts_with(root)
            || path.starts_with("/proc")
            || path.starts_with("/dev")
            || path.components().any(|c| c == Component::ParentDir)
            || !policy.can_read_local_path_with_cwd(path, root)
        {
            return Err(denied());
        }
        Ok(())
    };
    check(cwd)?;
    if !fs::metadata(cwd)?.is_dir() {
        return Err(io::Error::other("workdir is not a directory"));
    }
    let command = prepared.hook_command.as_str();
    if prepared.tty || !can_handle(command) {
        return Err(io::Error::other(
            "use a literal read-only command in the guest workspace",
        ));
    }
    let words: Vec<_> = command.split_ascii_whitespace().collect();
    let resolve = |arg: &str| -> io::Result<PathBuf> {
        let path = cwd.join(arg);
        check(&path)?;
        Ok(path)
    };
    match words.first().copied() {
        Some("pwd") => Ok(format!("{}\n", cwd.display()).into_bytes()),
        Some("cat") => {
            let mut numbered = false;
            let mut idx = 1;
            while idx < words.len() && words[idx] == "-n" {
                numbered = true;
                idx += 1;
            }
            let mut out = Vec::new();
            let mut line_no = 1usize;
            for arg in &words[idx..] {
                let data = read_bounded(&resolve(arg)?, LIMIT.saturating_sub(out.len()))?;
                if numbered {
                    for chunk in data.split_inclusive(|&b| b == b'\n') {
                        let prefix = format!("{line_no:6}\t");
                        if out.len() + prefix.len() + chunk.len() > LIMIT {
                            return Err(io::Error::other("output exceeds 64 KiB"));
                        }
                        out.extend_from_slice(prefix.as_bytes());
                        out.extend_from_slice(chunk);
                        line_no += 1;
                    }
                } else {
                    if out.len() + data.len() > LIMIT {
                        return Err(io::Error::other("output exceeds 64 KiB"));
                    }
                    out.extend_from_slice(&data);
                }
            }
            Ok(out)
        }
        Some("ls") => {
            let mut show_all = false;
            let mut long_fmt = false;
            let mut idx = 1;
            while idx < words.len() && words[idx].starts_with('-') {
                for b in words[idx][1..].bytes() {
                    if b == b'a' {
                        show_all = true;
                    } else if b == b'l' {
                        long_fmt = true;
                    }
                }
                idx += 1;
            }
            let target = resolve(words.get(idx).copied().unwrap_or("."))?;
            let meta = fs::symlink_metadata(&target)?;
            if meta.is_file() {
                let display = words.get(idx).copied().unwrap_or(".");
                let line = if long_fmt {
                    format!("- {:>8} {}\n", meta.len(), display)
                } else {
                    format!("{display}\n")
                };
                return Ok(line.into_bytes());
            }
            let mut rows = Vec::new();
            let mut total = 0;
            for entry in fs::read_dir(target)? {
                let entry = entry?;
                let name = entry.file_name().to_string_lossy().into_owned();
                if (!show_all && name.starts_with('.'))
                    || matches!(name.as_str(), ".git" | ".codex" | ".agents")
                    || check(&entry.path()).is_err()
                {
                    continue;
                }
                let line = if long_fmt {
                    let em = fs::symlink_metadata(entry.path())?;
                    let kind = if em.is_dir() { 'd' } else { '-' };
                    format!("{kind} {:>8} {name}\n", em.len())
                } else {
                    format!("{name}\n")
                };
                total += line.len();
                if total > LIMIT {
                    return Err(io::Error::other("listing exceeds 64 KiB"));
                }
                rows.push((name, line));
            }
            rows.sort_by(|a, b| a.0.cmp(&b.0));
            Ok(rows.into_iter().map(|(_, l)| l).collect::<String>().into_bytes())
        }
        Some("head" | "tail") => {
            let is_head = words[0] == "head";
            let (count, files) = parse_line_count(&words[1..])
                .ok_or_else(|| io::Error::other("invalid head/tail arguments"))?;
            let mut out = Vec::new();
            for (i, arg) in files.iter().enumerate() {
                let data = read_bounded(&resolve(arg)?, LIMIT)?;
                if files.len() > 1 {
                    let hdr = format!("{}==> {arg} <==\n", if i > 0 { "\n" } else { "" });
                    out.extend_from_slice(hdr.as_bytes());
                }
                let slices: Vec<&[u8]> = data.split_inclusive(|&b| b == b'\n').collect();
                let selected = if is_head {
                    &slices[..slices.len().min(count)]
                } else {
                    let start = slices.len().saturating_sub(count);
                    &slices[start..]
                };
                for s in selected {
                    if out.len() + s.len() > LIMIT {
                        return Err(io::Error::other("output exceeds 64 KiB"));
                    }
                    out.extend_from_slice(s);
                }
            }
            Ok(out)
        }
        Some("wc") => {
            let ((show_l, show_w, show_c), files) = parse_wc_args(&words[1..])
                .ok_or_else(|| io::Error::other("invalid wc arguments"))?;
            let mut out = String::new();
            let (mut tot_l, mut tot_w, mut tot_c) = (0usize, 0usize, 0usize);
            for arg in files {
                let data = read_bounded(&resolve(arg)?, 4 * 1024 * 1024)?;
                let lines = data.iter().filter(|&&b| b == b'\n').count();
                let words_cnt = String::from_utf8_lossy(&data).split_whitespace().count();
                let bytes = data.len();
                tot_l += lines;
                tot_w += words_cnt;
                tot_c += bytes;
                let mut cols = Vec::new();
                if show_l { cols.push(format!("{lines:>7}")); }
                if show_w { cols.push(format!("{words_cnt:>7}")); }
                if show_c { cols.push(format!("{bytes:>7}")); }
                out.push_str(&format!("{} {arg}\n", cols.join(" ")));
            }
            if files.len() > 1 {
                let mut cols = Vec::new();
                if show_l { cols.push(format!("{tot_l:>7}")); }
                if show_w { cols.push(format!("{tot_w:>7}")); }
                if show_c { cols.push(format!("{tot_c:>7}")); }
                out.push_str(&format!("{} total\n", cols.join(" ")));
            }
            Ok(out.into_bytes())
        }
        _ => Err(io::Error::other("unsupported read-only builtin")),
    }
}
