//! Snapshot the permitted workspace, run one command in an offline guest, then
//! publish its checked changes together using the primary volume's transaction.
use super::{SandboxAttempt, UnifiedExecRequest};
use base64::{Engine, engine::general_purpose::STANDARD as B64};
use codex_protocol::permissions::FileSystemSandboxPolicy;
use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, BTreeSet};
use std::ffi::CString;
use std::fs;
use std::io::{self, Read, Write};
use std::path::{Component, Path};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio_util::sync::CancellationToken;

type Files = BTreeMap<String, Option<String>>;
const BYTES: usize = 8 << 20;
const COUNT: usize = 1024;
const FRAME: usize = 12 << 20;

fn invalid(reason: &str) -> io::Error {
    io::Error::other(reason)
}

fn relative(path: &str) -> bool {
    !path.is_empty()
        && path.len() <= 480
        && !path.contains('\0')
        && path
            .split('/')
            .all(|part| !matches!(part, "" | "." | ".." | ".git" | ".codex" | ".agents"))
}

fn file_data(path: &Path, limit: usize) -> io::Result<String> {
    let f = fs::File::open(path)?;
    if f.metadata()?.len() > limit as u64 {
        return Err(invalid("workspace exceeds 8 MiB"));
    }
    let mut bytes = Vec::new();
    f.take(limit as u64 + 1).read_to_end(&mut bytes)?;
    if bytes.len() > limit {
        return Err(invalid("workspace exceeds 8 MiB"));
    }
    Ok(B64.encode(bytes))
}

struct Scope<'a> {
    root: &'a Path,
    policy: FileSystemSandboxPolicy,
    baseline: FileSystemSandboxPolicy,
}

impl Scope<'_> {
    fn readable(&self, path: &Path) -> bool {
        self.policy.can_read_local_path_with_cwd(path, self.root)
            && self.baseline.can_read_local_path_with_cwd(path, self.root)
    }
    fn snapshot(&self) -> io::Result<Files> {
        let mut files = Files::new();
        let mut dirs = vec![self.root.to_path_buf()];
        let mut remaining = BYTES;
        while let Some(dir) = dirs.pop() {
            for item in fs::read_dir(dir)? {
                let item = item?;
                let path = item.path();
                let key = path
                    .strip_prefix(self.root)
                    .map_err(io::Error::other)?
                    .to_str()
                    .ok_or_else(|| invalid("non-UTF-8 workspace path"))?;
                if !relative(key) {
                    if key
                        .split('/')
                        .any(|s| matches!(s, ".git" | ".codex" | ".agents"))
                    {
                        continue;
                    }
                    return Err(invalid("unsupported workspace path"));
                }
                if !self.readable(&path) {
                    continue;
                }
                if files.len() >= COUNT {
                    return Err(invalid("workspace exceeds 1024 entries"));
                }
                let meta = fs::symlink_metadata(&path)?;
                let value = if meta.is_dir() {
                    dirs.push(path.clone());
                    None
                } else if meta.is_file() {
                    let data = file_data(&path, remaining)?;
                    let n =
                        data.len() / 4 * 3 - data.bytes().rev().take_while(|&b| b == b'=').count();
                    remaining -= n;
                    Some(data)
                } else {
                    return Err(invalid("workspace contains a special file"));
                };
                files.insert(key.to_owned(), value);
            }
        }
        Ok(files)
    }
}

#[derive(Serialize)]
struct Request<'a> {
    files: &'a Files,
    cwd: &'a str,
    cmd: &'a str,
    timeout: u64,
}
#[derive(Deserialize)]
struct ResultFrame {
    status: u8,
    output: String,
    changed: Files,
    removed: Vec<String>,
}
#[derive(Deserialize)]
#[serde(untagged)]
enum Reply {
    Error { error: String },
    Done(ResultFrame),
}

async fn exchange(request: Request<'_>) -> io::Result<ResultFrame> {
    let body = serde_json::to_vec(&request)?;
    if body.len() > FRAME {
        return Err(invalid("shell request exceeds limit"));
    }
    let mut stream = TcpStream::connect("10.0.2.103:8766").await.map_err(|_| {
        invalid("isolated shell relay unavailable; relaunch with LIXINOS_CODEX_SHELL=1")
    })?;
    stream.write_u32(body.len() as u32).await?;
    stream.write_all(&body).await?;
    let size = stream.read_u32().await? as usize;
    if size > FRAME {
        return Err(invalid("shell response exceeds limit"));
    }
    let mut buf = vec![0; size];
    stream.read_exact(&mut buf).await?;
    match serde_json::from_slice(&buf)? {
        Reply::Done(value) => Ok(value),
        Reply::Error { error } => Err(invalid(&error)),
    }
}

unsafe extern "C" {
    fn lx_codex_batch(op: i32, path: *const libc::c_char, other: *const libc::c_char) -> i32;
}
fn batch(op: i32, path: &Path, other: &Path) -> io::Result<()> {
    let path = CString::new(path.as_os_str().as_encoded_bytes())?;
    let other = CString::new(other.as_os_str().as_encoded_bytes())?;
    // Both paths are borrowed only for this synchronous call.
    let err = unsafe { lx_codex_batch(op, path.as_ptr(), other.as_ptr()) };
    if err == 0 {
        Ok(())
    } else {
        Err(io::Error::from_raw_os_error(err))
    }
}

fn current(path: &Path) -> io::Result<Option<Option<String>>> {
    match fs::symlink_metadata(path) {
        Ok(m) if m.is_dir() => Ok(Some(None)),
        Ok(m) if m.is_file() => file_data(path, BYTES).map(|s| Some(Some(s))),
        Ok(_) => Err(invalid("unexpected file type")),
        Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(e),
    }
}

fn publish(scope: &Scope<'_>, original: &Files, reply: &ResultFrame) -> io::Result<()> {
    if reply.changed.is_empty() && reply.removed.is_empty() {
        return Ok(());
    }
    let names: BTreeSet<_> = reply.changed.keys().chain(reply.removed.iter()).collect();
    if names.len() != reply.changed.len() + reply.removed.len() || names.len() > COUNT * 2 {
        return Err(invalid("invalid shell changes"));
    }
    for key in &names {
        let path = scope.root.join(key);
        if !relative(key)
            || !scope.readable(&path)
            || !scope
                .policy
                .can_write_local_path_with_cwd(&path, scope.root)
            || !scope
                .baseline
                .can_write_local_path_with_cwd(&path, scope.root)
        {
            return Err(invalid(
                "workspace write denied by Codex permissions; changes discarded",
            ));
        }
    }
    let mut staged = BTreeMap::new();
    for (key, value) in &reply.changed {
        if let Some(encoded) = value {
            let bytes = B64.decode(encoded).map_err(io::Error::other)?;
            let mut f = tempfile::NamedTempFile::new_in("/tmp")?;
            f.write_all(&bytes)?;
            staged.insert(key, f.into_temp_path());
        }
    }
    let empty = Path::new("");
    batch(0, empty, empty)?;
    let result = (|| {
        for key in names {
            if current(&scope.root.join(key))?.as_ref() != original.get(key) {
                return Err(invalid(
                    "workspace changed during command; changes discarded",
                ));
            }
        }
        let mut removed: Vec<_> = reply.removed.iter().collect();
        removed.sort_by_key(|name| std::cmp::Reverse(name.split('/').count()));
        for key in removed {
            let dir = original
                .get(key)
                .ok_or_else(|| invalid("unexpected deletion"))?
                .is_none();
            batch(if dir { 259 } else { 3 }, &scope.root.join(key), empty)?;
        }
        for (key, value) in &reply.changed {
            let target = scope.root.join(key);
            match value {
                None => batch(1, &target, empty)?,
                Some(expected) => {
                    let temp = &staged[key];
                    if file_data(temp, BYTES)? != *expected {
                        return Err(invalid("staged file changed"));
                    }
                    batch(5, temp, &target)?;
                }
            }
        }
        batch(6, empty, empty)
    })();
    if result.is_err() {
        let _ = batch(7, empty, empty);
    } else {
        for temp in staged.values_mut() {
            temp.disable_cleanup(true);
        }
    }
    result
}

pub(super) async fn execute(
    req: &UnifiedExecRequest<'_>,
    attempt: &SandboxAttempt<'_>,
    cancel: &CancellationToken,
) -> io::Result<(i32, Vec<u8>)> {
    if req.prepared.tty {
        return Err(invalid("codex exec uses noninteractive shell jobs"));
    }
    let root = attempt
        .sandbox_cwd
        .to_abs_path()
        .map_err(io::Error::other)?;
    let cwd = req.prepared.cwd.to_abs_path().map_err(io::Error::other)?;
    let root = root.as_path();
    if root.parent().is_none() || root.starts_with("/proc") || root.starts_with("/dev")
        || root.components().any(|c| matches!(c, Component::Normal(p) if matches!(p.to_str(), Some(".codex" | ".git" | ".agents")))) {
        return Err(invalid("start Codex in a guest project directory"));
    }
    let workdir = cwd
        .as_path()
        .strip_prefix(root)
        .map_err(|_| invalid("outside the readable guest workspace"))?
        .to_str()
        .ok_or_else(|| invalid("invalid workdir"))?;
    let scope = Scope {
        root,
        policy: attempt.permissions.file_system_sandbox_policy(),
        baseline: req
            .prepared
            .turn_environment
            .permission_profile_with_workspace_roots()
            .file_system_sandbox_policy(),
    };
    if (!workdir.is_empty() && !relative(workdir))
        || !scope.readable(root)
        || !scope.readable(cwd.as_path())
    {
        return Err(invalid("outside the readable guest workspace"));
    }
    let files = scope.snapshot()?;
    let timeout = req
        .prepared
        .completion_timeout
        .map_or(60, |d| d.as_secs().clamp(1, 120));
    let request = Request {
        files: &files,
        cwd: workdir,
        cmd: &req.prepared.hook_command,
        timeout,
    };
    let result = tokio::select! {
        _ = cancel.cancelled() => return Err(invalid("shell job cancelled")),
        reply = tokio::time::timeout(std::time::Duration::from_secs(timeout + 100), exchange(request)) =>
            reply.map_err(|_| invalid("shell relay timed out"))??,
    };
    if cancel.is_cancelled() {
        return Err(invalid("shell job cancelled"));
    }
    publish(&scope, &files, &result)?;
    Ok((i32::from(result.status), result.output.into_bytes()))
}
