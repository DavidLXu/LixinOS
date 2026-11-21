// SFTP v3 over the authenticated SSH session channel.
use super::*;
use alloc::{ffi::CString, format, string::String as Text, vec::Vec};
use core::alloc::{GlobalAlloc, Layout};
use sunset::{ChanHandle, Client, Result};

unsafe extern "C" {
    fn memalign(align: usize, size: usize) -> *mut u8;
    fn free(p: *mut u8);
    fn lx_open(path: *const i8) -> i32;
    fn lx_read(fd: i32, p: *mut u8, n: i32) -> i32;
    fn lx_close(fd: i32) -> i32;
    fn lx_stat(path: *const i8) -> isize;
    fn lx_dir(path: *const i8, index: u32, name: *mut u8) -> isize;
    fn lx_save_buffer(path: *const i8, buf: *const u8, len: u32) -> i32;
    fn ssh_mkdir(path: *const i8) -> i32;
}
struct Heap;
unsafe impl GlobalAlloc for Heap {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        unsafe { memalign(l.align().max(8), l.size()) }
    }
    unsafe fn dealloc(&self, p: *mut u8, _: Layout) {
        unsafe { free(p) }
    }
}
#[global_allocator]
static HEAP: Heap = Heap;

// The syscall mailbox and an SFTP read/write request have independent limits.
const MAILBOX: usize = 4096;
const FILE_BLOCK: usize = 32 * 1024;
const MAX_PACKET: usize = 256 * 1024;
const MAX_FILE: usize = 100 * 1024 * 1024;
fn invalid() -> Error {
    Error::msg("invalid SFTP reply")
}
type Ssh<'a> = Runner<'a, Client>;
struct Wire<'a, 'b> {
    ssh: &'a mut Ssh<'b>,
    channel: &'a ChanHandle,
    socket: i32,
    pending: [u8; MAILBOX],
    start: usize,
    end: usize,
    id: u32,
    replace: bool,
    broken: bool,
}
impl Wire<'_, '_> {
    fn pump(&mut self) -> Result<bool> {
        let mut moved = match self.ssh.progress()? {
            Event::Cli(CliEvent::Defunct | CliEvent::SessionExit(_)) => {
                return Err(Error::msg("SFTP subsystem closed"));
            }
            Event::None => false,
            _ => true,
        };
        let out = self.ssh.output_buf();
        if !out.is_empty() {
            match unsafe { lx_tcp_write(self.socket, out.as_ptr(), out.len().min(MAILBOX) as i32) }
            {
                n if n > 0 => {
                    self.ssh.consume_output(n as usize);
                    moved = true;
                }
                -2 => (),
                _ => return Err(Error::msg("SFTP socket write failed")),
            }
        }
        if self.ssh.is_input_ready() {
            if self.start == self.end {
                match unsafe { lx_tcp_read(self.socket, self.pending.as_mut_ptr(), MAILBOX as i32) }
                {
                    n if n > 0 => {
                        self.start = 0;
                        self.end = n as usize;
                    }
                    -2 => (),
                    _ => return Err(Error::msg("SFTP connection closed")),
                }
            }
            if self.start < self.end {
                let n = self.ssh.input(&self.pending[self.start..self.end])?;
                self.start += n;
                moved |= n != 0;
            }
        }
        if self.ssh.is_channel_closed(self.channel) {
            return Err(Error::msg("SFTP channel closed"));
        }
        Ok(moved)
    }
    fn exchange(&mut self, request: &[u8]) -> Result<Vec<u8>> {
        let mut sent = 0;
        let mut header = [0; 4];
        let mut got = 0;
        let mut reply = Vec::new();
        let mut idle = 0;
        loop {
            let mut moved = self.pump()?;
            if sent < request.len() {
                let n = self
                    .ssh
                    .write_channel(self.channel, ChanData::Normal, &request[sent..])?;
                moved |= n != 0;
                sent += n;
            }
            if self.ssh.read_channel_ready().is_some() {
                let dst = if got < 4 {
                    &mut header[got..]
                } else {
                    &mut reply[got - 4..]
                };
                let (n, kind) = self.ssh.read_channel_either(self.channel, dst)?;
                moved |= n != 0;
                if kind == ChanData::Normal {
                    got += n;
                    if got == 4 && reply.is_empty() {
                        let len = u32::from_be_bytes(header) as usize;
                        if !(5..=MAX_PACKET).contains(&len) {
                            return Err(invalid());
                        }
                        reply.resize(len, 0);
                    }
                    if got == 4 + reply.len() && got > 4 {
                        return Ok(reply);
                    }
                }
            } else if self.ssh.is_channel_eof(self.channel) {
                return Err(Error::msg("SFTP EOF"));
            }
            if moved {
                idle = 0;
            } else {
                if unsafe { ssh_key(2) } == 3 {
                    return Err(Error::msg("transfer interrupted"));
                }
                idle += 1;
                if idle > 15000 {
                    return Err(Error::msg("SFTP timed out"));
                }
            }
        }
    }
    fn request(&mut self, kind: u8, payload: &[u8]) -> Result<Reply> {
        if self.broken {
            return Err(Error::msg("SFTP connection lost"));
        }
        self.broken = true;
        self.id = self.id.wrapping_add(1);
        let mut frame = Vec::new();
        num(&mut frame, 5 + payload.len() as u32);
        frame.push(kind);
        num(&mut frame, self.id);
        frame.extend(payload);
        let bytes = self.exchange(&frame)?;
        if bytes[1..5] != self.id.to_be_bytes() {
            return Err(invalid());
        }
        self.broken = false;
        Ok(Reply(bytes))
    }
    fn path(&mut self, kind: u8, path: &str) -> Result<Reply> {
        self.request(kind, &string(path.as_bytes()))
    }
    fn close(&mut self, handle: &[u8]) -> Result<()> {
        self.request(4, &string(handle))?.ok()
    }
}
struct Reply(Vec<u8>);
impl Reply {
    fn body(&self, kind: u8) -> Result<&[u8]> {
        if self.0[0] == kind {
            Ok(&self.0[5..])
        } else {
            Err(Error::msg(
                "unexpected SFTP response or remote operation failed",
            ))
        }
    }
    fn status(&self) -> Option<u32> {
        read_num(&mut self.body(101).ok()?).ok()
    }
    fn ok(&self) -> Result<()> {
        if self.status() == Some(0) {
            Ok(())
        } else {
            Err(Error::msg("remote operation failed"))
        }
    }
    fn handle(&self) -> Result<Vec<u8>> {
        Ok(field(&mut self.body(102)?)?.to_vec())
    }
}
fn num(v: &mut Vec<u8>, n: u32) {
    v.extend(n.to_be_bytes());
}
fn string(s: &[u8]) -> Vec<u8> {
    let mut v = Vec::with_capacity(4 + s.len());
    num(&mut v, s.len() as u32);
    v.extend(s);
    v
}
fn take<'a>(s: &mut &'a [u8], n: usize) -> Result<&'a [u8]> {
    let (v, rest) = s.split_at_checked(n).ok_or(invalid())?;
    *s = rest;
    Ok(v)
}
fn read_num(s: &mut &[u8]) -> Result<u32> {
    Ok(u32::from_be_bytes(take(s, 4)?.try_into().unwrap()))
}
fn field<'a>(s: &mut &'a [u8]) -> Result<&'a [u8]> {
    let n = read_num(s)?;
    take(s, n as usize)
}
fn attrs(s: &mut &[u8]) -> Result<(Option<u64>, u32)> {
    let flags = read_num(s)?;
    let len = if flags & 1 != 0 {
        Some(u64::from_be_bytes(take(s, 8)?.try_into().unwrap()))
    } else {
        None
    };
    if flags & 2 != 0 {
        take(s, 8)?;
    }
    let mode = if flags & 4 != 0 { read_num(s)? } else { 0 };
    if flags & 8 != 0 {
        take(s, 8)?;
    }
    if flags & (1 << 31) != 0 {
        for _ in 0..read_num(s)? {
            field(s)?;
            field(s)?;
        }
    }
    Ok((len, mode & 0o170000))
}
fn local(path: &str) -> Result<CString> {
    if path.is_empty() || path.len() > 512 {
        return Err(Error::msg("invalid local path"));
    }
    CString::new(path).map_err(|_| invalid())
}
fn child(dir: &str, name: &str) -> Result<Text> {
    if name.is_empty() || matches!(name, "." | "..") || name.contains(['/', '\0']) {
        return Err(invalid());
    }
    let path = format!("{}/{name}", dir.trim_end_matches('/'));
    if path.len() > 512 {
        return Err(Error::msg("path too long"));
    }
    Ok(path)
}
fn transfer(
    w: &mut Wire,
    upload: bool,
    recursive: bool,
    src: &str,
    dst: &str,
    depth: usize,
) -> Result<()> {
    if depth > 32 {
        return Err(Error::msg("directory nesting exceeds 32"));
    }
    let lp = local(if upload { src } else { dst })?;
    let (size, kind) = if upload {
        match unsafe { lx_stat(lp.as_ptr().cast()) } {
            -2 => (None, 0o040000),
            n if n >= 0 => (Some(n as u64), 0o100000),
            _ => return Err(Error::msg("local source unavailable")),
        }
    } else {
        attrs(&mut w.path(7, src)?.body(105)?)?
    };
    if kind == 0o040000 {
        if !recursive {
            return Err(Error::msg("use -r for a directory"));
        }
        if upload {
            let existing = w.path(7, dst)?;
            if existing.status() == Some(2) {
                let mut req = string(dst.as_bytes());
                num(&mut req, 0);
                w.request(14, &req)?.ok()?;
            } else if attrs(&mut existing.body(105)?)?.1 != 0o040000 {
                return Err(Error::msg("remote destination is not a directory"));
            }
            for i in 0.. {
                let mut buf = [0; 513];
                match unsafe { lx_dir(lp.as_ptr().cast(), i, buf.as_mut_ptr()) } {
                    -1 => break,
                    n if n >= 0 || n == -2 => (),
                    _ => return Err(Error::msg("local listing failed")),
                }
                let n = buf.iter().position(|&x| x == 0).ok_or(invalid())?;
                let name = str::from_utf8(&buf[..n]).map_err(|_| invalid())?;
                transfer(
                    w,
                    true,
                    true,
                    &child(src, name)?,
                    &child(dst, name)?,
                    depth + 1,
                )?;
            }
        } else {
            if unsafe { lx_stat(lp.as_ptr().cast()) } != -2
                && unsafe { ssh_mkdir(lp.as_ptr().cast()) } < 0
            {
                return Err(Error::msg("cannot create local directory"));
            }
            let h = w.path(11, src)?.handle()?;
            let result = (|| {
                loop {
                    let batch = w.request(12, &string(&h))?;
                    if batch.status() == Some(1) {
                        break;
                    }
                    let mut rows = batch.body(104)?;
                    let count = read_num(&mut rows)?;
                    if count == 0 {
                        return Err(invalid());
                    }
                    for _ in 0..count {
                        let name = str::from_utf8(field(&mut rows)?).map_err(|_| invalid())?;
                        field(&mut rows)?;
                        let (_, mode) = attrs(&mut rows)?;
                        if matches!(name, "." | "..") || !matches!(mode, 0 | 0o040000 | 0o100000) {
                            continue;
                        }
                        transfer(
                            w,
                            false,
                            true,
                            &child(src, name)?,
                            &child(dst, name)?,
                            depth + 1,
                        )?;
                    }
                }
                Ok(())
            })();
            let close = w.close(&h);
            result?;
            close?;
        }
        return Ok(());
    }
    if kind != 0o100000 {
        return Err(Error::msg(
            "only regular files and directories are supported",
        ));
    }
    let len = size
        .filter(|n| *n <= MAX_FILE as u64)
        .ok_or(Error::msg("file too large or missing size"))? as usize;
    if upload {
        let (dir, _) = dst.rsplit_once('/').unwrap_or((".", dst));
        let mut nonce = [0; 8];
        getrandom::fill(&mut nonce).map_err(|_| Error::msg("entropy unavailable"))?;
        let tmp = child(
            dir,
            &format!(".lixin-sftp-{:016x}", u64::from_ne_bytes(nonce)),
        )?;
        let mut req = string(tmp.as_bytes());
        num(&mut req, 2 | 8 | 32);
        num(&mut req, 0);
        let h = w.request(3, &req)?.handle()?;
        let fd = unsafe { lx_open(lp.as_ptr().cast()) };
        let result = (|| {
            if fd < 0 {
                return Err(Error::msg("cannot open local file"));
            }
            let mut offset = 0;
            let mut req = string(&h);
            let handle_end = req.len();
            req.reserve(12 + FILE_BLOCK);
            while offset < len {
                let count = FILE_BLOCK.min(len - offset);
                req.truncate(handle_end);
                req.extend((offset as u64).to_be_bytes());
                num(&mut req, count as u32);
                let payload = req.len();
                req.resize(payload + count, 0);
                let mut read = 0;
                while read < count {
                    let n = unsafe {
                        lx_read(
                            fd,
                            req[payload + read..].as_mut_ptr(),
                            (count - read).min(MAILBOX) as i32,
                        )
                    };
                    if n <= 0 {
                        return Err(Error::msg("local read failed"));
                    }
                    read += n as usize;
                }
                w.request(6, &req)?.ok()?;
                offset += count;
            }
            Ok(())
        })();
        if fd >= 0 {
            unsafe {
                lx_close(fd);
            }
        }
        let closed = w.close(&h);
        let publish = result.and(closed).and_then(|_| {
            let mut req = if w.replace {
                string(b"posix-rename@openssh.com")
            } else {
                Vec::new()
            };
            req.extend(string(tmp.as_bytes()));
            req.extend(string(dst.as_bytes()));
            w.request(if w.replace { 200 } else { 18 }, &req)?.ok()
        });
        if publish.is_err() {
            let _ = w.path(13, &tmp);
        }
        publish
    } else {
        let mut data = Vec::new();
        data.try_reserve_exact(len)
            .map_err(|_| Error::msg("not enough memory for file"))?;
        let mut req = string(src.as_bytes());
        num(&mut req, 1);
        num(&mut req, 0);
        let h = w.request(3, &req)?.handle()?;
        let result = (|| {
            loop {
                let mut req = string(&h);
                req.extend((data.len() as u64).to_be_bytes());
                num(&mut req, FILE_BLOCK as u32);
                let packet = w.request(5, &req)?;
                if packet.status() == Some(1) {
                    break;
                }
                let b = field(&mut packet.body(103)?)?;
                if b.is_empty() || b.len() > FILE_BLOCK || b.len() > len - data.len() {
                    return Err(invalid());
                }
                data.extend(b);
            }
            if data.len() != len {
                return Err(Error::msg("remote file changed during transfer"));
            }
            Ok(())
        })();
        let close = w.close(&h);
        result?;
        close?;
        if unsafe { lx_save_buffer(lp.as_ptr().cast(), data.as_ptr(), len as u32) } < 0 {
            return Err(Error::msg("local save failed"));
        }
        Ok(())
    }
}
// Quoting follows the familiar single/double quote and backslash forms.
fn words(line: &str) -> Option<Vec<Text>> {
    let mut args = Vec::new();
    let mut word = Text::new();
    let mut quote = None;
    let mut started = false;
    let mut chars = line.chars();
    while let Some(c) = chars.next() {
        if quote == Some(c) {
            quote = None;
        } else if c == '\\' && quote != Some('\'') {
            word.push(chars.next()?);
        } else if matches!(c, '\'' | '"') && quote.is_none() {
            quote = Some(c);
        } else if c.is_ascii_whitespace() && quote.is_none() {
            if started {
                args.push(core::mem::take(&mut word));
                started = false;
            }
            continue;
        } else {
            word.push(c);
        }
        started = true;
    }
    if started {
        args.push(word);
    }
    quote.is_none().then_some(args)
}
pub(super) fn run(ssh: &mut Ssh, channel: &ChanHandle, socket: i32, pending: &[u8]) -> Result<u8> {
    let mut w = Wire {
        ssh,
        channel,
        socket,
        pending: [0; MAILBOX],
        start: 0,
        end: pending.len(),
        id: 0,
        replace: false,
        broken: false,
    };
    w.pending[..pending.len()].copy_from_slice(pending);
    let hello = w.exchange(&[0, 0, 0, 5, 1, 0, 0, 0, 3])?;
    if hello[..5] != [2, 0, 0, 0, 3] {
        return Err(Error::msg("server does not support SFTP v3"));
    }
    let mut extensions = &hello[5..];
    while !extensions.is_empty() {
        let key = field(&mut extensions)?;
        let val = field(&mut extensions)?;
        w.replace |= key == b"posix-rename@openssh.com" && val == b"1";
    }
    loop {
        output(b"sftp> ");
        let mut buf = [0; 1200];
        let cmd = line(&mut buf, true)?;
        let Some(words) = words(cmd) else {
            output(b"unfinished quote or escape\r\n");
            continue;
        };
        let mut args = words.iter().map(Text::as_str);
        let upload = match args.next() {
            None => continue,
            Some("quit" | "bye" | "exit") => return Ok(0),
            Some("put") => true,
            Some("get") => false,
            _ => {
                output(b"get [-r] SOURCE [DEST], put [-r] SOURCE [DEST], quit\r\n");
                continue;
            }
        };
        let mut src = args.next();
        let recursive = src == Some("-r");
        if recursive {
            src = args.next();
        }
        let Some(src) = src else {
            output(b"source path required\r\n");
            continue;
        };
        let base = src.trim_end_matches('/').rsplit('/').next().unwrap_or(src);
        let dest_arg = args.next();
        let dst = dest_arg.unwrap_or(base);
        if args.next().is_some() || dst.is_empty() {
            output(b"expected source and destination paths\r\n");
            continue;
        }
        let result = (|| {
            let is_dir = if dest_arg.is_none() {
                false
            } else if upload {
                let reply = w.path(7, dst)?;
                reply.0[0] == 105 && attrs(&mut reply.body(105)?)?.1 == 0o040000
            } else {
                unsafe { lx_stat(local(dst)?.as_ptr().cast()) == -2 }
            };
            let nested;
            let dest = if is_dir {
                nested = child(dst, base)?;
                &nested
            } else {
                dst
            };
            transfer(&mut w, upload, recursive, src, dest, 0)
        })();
        match result {
            Ok(()) => output(b"Transfer complete.\r\n"),
            Err(e) if w.broken => return Err(e),
            Err(e) => {
                writeln!(Console, "sftp: {e}\r").unwrap();
            }
        }
    }
}
