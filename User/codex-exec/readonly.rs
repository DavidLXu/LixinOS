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
    if prepared.tty
        || command
            .chars()
            .any(|c| !(c.is_alphanumeric() || " ./_-".contains(c)))
    {
        return Err(io::Error::other(
            "use a literal pwd, ls [path], or cat file in the guest workspace",
        ));
    }
    let words: Vec<_> = command.split_ascii_whitespace().collect();
    let resolve = |arg: &str| -> io::Result<PathBuf> {
        if arg.starts_with('-') {
            return Err(io::Error::other("command options are unsupported"));
        }
        let path = cwd.join(arg);
        check(&path)?;
        Ok(path)
    };
    match words.as_slice() {
        ["pwd"] => Ok(format!("{}\n", cwd.display()).into_bytes()),
        ["cat", arg] => {
            let file = fs::File::open(resolve(arg)?)?;
            if !file.metadata()?.is_file() {
                return Err(io::Error::other("cat requires a regular file"));
            }
            let mut data = Vec::new();
            file.take((LIMIT + 1) as u64).read_to_end(&mut data)?;
            if data.len() > LIMIT {
                return Err(io::Error::other("file exceeds 64 KiB"));
            }
            Ok(data)
        }
        ["ls"] | ["ls", _] => {
            let path = resolve(words.get(1).unwrap_or(&"."))?;
            let mut names = Vec::new();
            let mut total = 0;
            for entry in fs::read_dir(path)? {
                let entry = entry?;
                let name = entry.file_name().to_string_lossy().into_owned();
                if name.starts_with('.') || check(&entry.path()).is_err() {
                    continue;
                }
                total += name.len() + 1;
                if total > LIMIT {
                    return Err(io::Error::other("listing exceeds 64 KiB"));
                }
                names.push(name + "\n");
            }
            names.sort();
            Ok(names.concat().into_bytes())
        }
        _ => Err(io::Error::other(
            "LixinOS supports pwd, ls [path], and cat file; general shell execution is unavailable",
        )),
    }
}
