#![no_std]
extern crate alloc;
mod sftp;
use base64ct::{Base64, Base64Unpadded, Encoding};
use core::{
    fmt::{self, Write},
    net::Ipv4Addr,
    str,
};
use heapless::String;
use sha2::{Digest, Sha256};
use sunset::{ChanData, CliEvent, CliSessionExit, Error, Event, Pty, Runner};
use zeroize::Zeroizing;

unsafe extern "C" {
    fn lx_tcp_connect(ip: *const u8, port: i32) -> i32;
    fn lx_tcp_read(fd: i32, data: *mut u8, n: i32) -> i32;
    fn lx_tcp_write(fd: i32, data: *const u8, n: i32) -> i32;
    fn lx_tcp_close(fd: i32) -> i32;
    fn lx_geometry() -> u64;
    fn ssh_key(ms: u32) -> i32;
    fn lx_random(p: *mut u8, n: i32) -> i32;
    fn ssh_output(p: *const u8, n: usize);
    fn ssh_pin(path: *const u8, fingerprint: *const u8, n: usize, create: i32) -> i32;
    fn ssh_abort() -> !;
    fn ssh_file(path: *const u8, data: *mut u8, capacity: usize) -> i32;
    fn ssh_create_seed(data: *const u8) -> i32;
}
#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    unsafe { ssh_abort() }
}
#[unsafe(no_mangle)]
unsafe extern "Rust" fn __getrandom_v03_custom(
    p: *mut u8,
    n: usize,
) -> Result<(), getrandom::Error> {
    if n <= i32::MAX as usize && unsafe { lx_random(p, n as i32) } == 0 {
        Ok(())
    } else {
        Err(getrandom::Error::UNSUPPORTED)
    }
}
fn output(bytes: &[u8]) {
    unsafe { ssh_output(bytes.as_ptr(), bytes.len()) }
}

struct TerminalModes(bool);
impl TerminalModes {
    fn restore(&mut self) {
        if core::mem::take(&mut self.0) {
            output(b"\x1b[?1049l\x1b[?25h\x1b[?1l\x1b[?7h\x1b(B\x0f\x1b[0m");
        }
    }
}
impl Drop for TerminalModes {
    fn drop(&mut self) {
        self.restore();
    }
}
struct Console;
impl fmt::Write for Console {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        output(s.as_bytes());
        Ok(())
    }
}
fn line(buf: &mut [u8], echo: bool) -> sunset::Result<&str> {
    let mut n = 0;
    let mut overflow = false;
    loop {
        let byte = unsafe { ssh_key(100) };
        match byte {
            3 | 4 => return Err(Error::msg("cancelled")),
            10 | 13 => {
                output(b"\r\n");
                if overflow {
                    return Err(Error::msg("input too long"));
                }
                return str::from_utf8(&buf[..n]).map_err(|_| Error::msg("invalid UTF-8"));
            }
            _ if overflow => {}
            8 | 127 if n != 0 => {
                n -= 1;
                if echo {
                    output(b"\x08 \x08");
                }
            }
            32..=255 if byte != 127 => {
                if n == buf.len() {
                    overflow = true;
                    continue;
                }
                buf[n] = byte as u8;
                if echo {
                    output(&buf[n..=n]);
                }
                n += 1;
            }
            -1 => return Err(Error::msg("terminal unavailable")),
            _ => {}
        }
    }
}
struct Target<'a> {
    user: &'a str,
    ip: Ipv4Addr,
    port: u16,
}
impl<'a> Target<'a> {
    fn parse(args: &'a [u8]) -> Option<Self> {
        let mut words = str::from_utf8(args).ok()?.strip_suffix('\0')?.split('\0');
        let first = words.next()?;
        let (addr, port) = if matches!(first, "-p" | "-P") {
            let port = words.next()?.parse::<u16>().ok()?;
            (words.next()?, port)
        } else {
            (first, 22)
        };
        let (user, host) = addr.split_once('@')?;
        if words.next().is_some()
            || port == 0
            || user.is_empty()
            || user.len() > sunset::config::MAX_USERNAME
            || !user.bytes().all(|b| b.is_ascii_graphic())
        {
            return None;
        }
        Some(Self {
            user,
            ip: host.parse().ok()?,
            port,
        })
    }
}
const IDENTITY: &[u8] = b"/.ssh-gpu20-key\0";
const PROFILE: &[u8] = b"/.ssh-gpu20\0";

// Generate only on explicit enrollment. A lost key must not silently replace
// an identity that is already authorized on a server.
fn identity(create: bool) -> sunset::Result<sunset::SignKey> {
    let mut seed = Zeroizing::new([0u8; 32]);
    let read = |buf: &mut [u8]| unsafe { ssh_file(IDENTITY.as_ptr(), buf.as_mut_ptr(), buf.len()) };
    let mut count = read(&mut seed[..]);
    if count == -3 && create {
        getrandom::fill(&mut seed[..]).map_err(|_| Error::msg("virtio-rng unavailable"))?;
        unsafe {
            ssh_create_seed(seed.as_ptr());
        }
        count = read(&mut seed[..]);
    }
    if count != 32 {
        return Err(Error::msg("GPU20 identity missing or invalid"));
    }
    Ok(sunset::SignKey::Ed25519(
        sunset::ed25519_dalek::SigningKey::from_bytes(&seed),
    ))
}

fn public_key() -> sunset::Result<u8> {
    let key = identity(true)?;
    let mut wire = [0; 96];
    let size = sunset::sshwire::write_ssh(&mut wire, &key.pubkey())?;
    let mut encoded = [0; 128];
    let field = Base64::encode(&wire[..size], &mut encoded)
        .map_err(|_| Error::msg("public key encoding failed"))?;
    write!(Console, "ssh-ed25519 {field} LixinOS-gpu20\r\n").unwrap();
    Ok(0)
}

fn gpu20(transfer: bool) -> sunset::Result<u8> {
    let mut buf = [0; 160];
    let n = unsafe { ssh_file(PROFILE.as_ptr(), buf.as_mut_ptr(), buf.len()) };
    if n < 0 {
        return Err(Error::msg(
            "missing /.ssh-gpu20 profile; enroll this disk first",
        ));
    }
    let mut fields = str::from_utf8(&buf[..n as usize])
        .map_err(|_| Error::msg("invalid GPU20 profile"))?
        .split_ascii_whitespace();
    let (Some(user), Some(pin), None) = (fields.next(), fields.next(), fields.next()) else {
        return Err(Error::msg(
            "expected username and SHA256 host fingerprint in /.ssh-gpu20",
        ));
    };
    let fingerprint = pin
        .strip_prefix("SHA256:")
        .filter(|p| p.len() == 43)
        .ok_or(Error::msg("invalid GPU20 pin"))?;
    session(
        Target {
            user,
            ip: Ipv4Addr::new(10, 0, 2, 100),
            port: 22,
        },
        Some((fingerprint, identity(false)?)),
        transfer,
    )
}

struct Socket(i32);
impl Drop for Socket {
    fn drop(&mut self) {
        unsafe {
            lx_tcp_close(self.0);
        }
    }
}
fn verify_host(
    event: sunset::event::CheckHostkey<'_, '_>,
    path: &str,
    t: &Target<'_>,
    expected: Option<&str>,
) -> sunset::Result<()> {
    let mut wire = [0; 256];
    let n = sunset::sshwire::write_ssh(&mut wire, &event.hostkey()?)?;
    let digest = Sha256::digest(&wire[..n]);
    let mut b64 = [0; 44];
    let fingerprint = Base64Unpadded::encode(&digest, &mut b64).unwrap();
    if let Some(expected) = expected {
        if expected != fingerprint {
            return Err(Error::msg(
                "GPU20 host key differs from enrolled fingerprint",
            ));
        }
        return event.accept();
    }
    let pin = |create| unsafe {
        ssh_pin(
            path.as_ptr(),
            fingerprint.as_ptr(),
            fingerprint.len(),
            create,
        )
    };
    let fresh = match pin(0) {
        1 => false,
        0 => {
            write!(
                Console,
                "Host {}:{}  SHA256:{}\r\nTrust this host (yes/no)? ",
                t.ip, t.port, fingerprint
            )
            .unwrap();
            if line(&mut [0; 8], true)? != "yes" {
                return Err(Error::msg("host not trusted"));
            }
            true
        }
        _ => return Err(Error::msg("host key changed or trust record unreadable")),
    };
    event.accept()?;
    if fresh && pin(1) != 1 {
        return Err(Error::msg("could not save verified host key"));
    }
    Ok(())
}

fn session(
    t: Target,
    profile: Option<(&str, sunset::SignKey)>,
    transfer: bool,
) -> sunset::Result<u8> {
    let mut terminal = TerminalModes(false);
    let mut addr = String::<16>::new();
    write!(addr, "{}\0", t.ip).unwrap();
    let fd = unsafe { lx_tcp_connect(addr.as_ptr(), t.port.into()) };
    if fd < 0 {
        return Err(Error::msg("TCP connection failed"));
    }
    let _socket = Socket(fd);
    // Probe entropy before initiating key exchange. Never substitute timer values.
    getrandom::fill(&mut [0u8; 1]).map_err(|_| Error::msg("virtio-rng unavailable"))?;
    let mut path = String::<64>::new();
    write!(path, "/.ssh-host-{}-{}\0", t.ip, t.port).unwrap();
    let mut incoming = [0; 35000];
    let mut outgoing = [0; 35000];
    let mut ssh = Runner::new_client(&mut incoming, &mut outgoing);
    let mut buf = [0; 4096];
    let mut start = 0;
    let mut end = 0;
    let mut chan = None;
    let mut opened = false;
    let mut pty_size = (0, 0);
    let mut status = None;
    let mut idle = 0;
    let mut bol = true;
    let mut escaped = false;
    let (expected, mut key) = profile.unzip();
    let mut attempted = false;
    loop {
        let mut moved = true;
        let mut authenticated = false;
        match ssh.progress()? {
            Event::None => moved = false,
            Event::Progressed | Event::Cli(CliEvent::PollAgain) => {}
            Event::Cli(CliEvent::Hostkey(e)) => {
                verify_host(e, &path, &t, expected)?;
            }
            Event::Cli(CliEvent::Username(e)) => e.username(t.user)?,
            Event::Cli(CliEvent::Password(e)) => {
                if expected.is_some() {
                    return Err(Error::msg("GPU20 rejected the guest key"));
                }
                if attempted {
                    return Err(Error::msg("authentication failed"));
                }
                attempted = true;
                write!(Console, "{}@{} password: ", t.user, t.ip).unwrap();
                let mut secret = Zeroizing::new([0u8; 256]);
                e.password(line(&mut secret[..], false)?)?;
            }
            Event::Cli(CliEvent::Pubkey(e)) => match key.take() {
                Some(key) => e.pubkey(key)?,
                None => e.skip()?,
            },
            Event::Cli(CliEvent::AgentSign(e)) => e.skip()?,
            Event::Cli(CliEvent::Banner(_)) => {}
            Event::Cli(CliEvent::Authenticated) => {
                authenticated = true;
            }
            Event::Cli(CliEvent::SessionOpened(mut e)) => {
                if transfer {
                    e.subsystem("sftp")?;
                } else {
                    let dims = unsafe { lx_geometry() };
                    pty_size = ((dims & 65535) as u32, ((dims >> 16) & 65535) as u32);
                    e.pty(Pty {
                        term: "xterm".try_into().unwrap(),
                        cols: pty_size.0,
                        rows: pty_size.1,
                        width: 0,
                        height: 0,
                        modes: Default::default(),
                    })?;
                    e.shell()?;
                    terminal.0 = true;
                }
                opened = true;
            }
            Event::Cli(CliEvent::SessionExit(e)) => {
                status = Some(match e {
                    CliSessionExit::Status(n) => n.min(255) as u8,
                    _ => 255,
                });
            }
            Event::Cli(CliEvent::Defunct) => return Ok(status.unwrap_or(255)),
            Event::Serv(_) => return Err(Error::msg("unexpected server event")),
        }
        if authenticated {
            chan = Some(ssh.open_client_session()?);
        }
        let out = ssh.output_buf();
        if !out.is_empty() {
            let n = unsafe { lx_tcp_write(fd, out.as_ptr(), out.len().min(4096) as i32) };
            if n > 0 {
                ssh.consume_output(n as usize);
                moved = true;
            } else if n != -2 {
                return Err(Error::msg("TCP write failed"));
            }
        }
        if ssh.is_input_ready() {
            if start == end {
                let n = unsafe { lx_tcp_read(fd, buf.as_mut_ptr(), buf.len() as i32) };
                match n {
                    0 => {
                        return status.ok_or(Error::msg("connection closed"));
                    }
                    -2 => {}
                    n if n > 0 => {
                        start = 0;
                        end = n as usize;
                    }
                    _ => return Err(Error::msg("TCP read failed")),
                }
            }
            if start < end {
                let n = ssh.input(&buf[start..end])?;
                start += n;
                moved |= n > 0;
            }
        }
        if let Some(c) = &chan {
            if transfer && opened {
                return sftp::run(&mut ssh, c, fd, &buf[start..end]);
            }
            if opened {
                let dims = unsafe { lx_geometry() };
                let size = ((dims & 65535) as u32, ((dims >> 16) & 65535) as u32);
                if size != pty_size
                    && ssh
                        .term_window_change(
                            c,
                            &sunset::packets::WinChange {
                                cols: size.0,
                                rows: size.1,
                                width: 0,
                                height: 0,
                            },
                        )
                        .is_ok()
                {
                    pty_size = size;
                    moved = true;
                }
            }
            if ssh.read_channel_ready().is_some() {
                let mut data = [0; 1024];
                let (n, _) = ssh.read_channel_either(c, &mut data)?;
                output(&data[..n]);
                moved |= n > 0;
            }
            if ssh.is_channel_closed(c) {
                return Ok(status.unwrap_or(255));
            }
            if opened
                && ssh
                    .write_channel_ready(c, ChanData::Normal)?
                    .is_some_and(|n| n >= 2)
            {
                let byte = unsafe { ssh_key(if moved { 0 } else { 2 }) };
                if let Ok(b) = u8::try_from(byte) {
                    if escaped {
                        escaped = false;
                        if b == b'.' {
                            terminal.restore();
                            output(b"\r\nDisconnected.\r\n");
                            return Ok(0);
                        }
                        ssh.write_channel(c, ChanData::Normal, b"~")?;
                        if b != b'~' {
                            ssh.write_channel(c, ChanData::Normal, &[b])?;
                        }
                    } else if bol && b == b'~' {
                        escaped = true;
                        continue;
                    } else {
                        ssh.write_channel(c, ChanData::Normal, &[b])?;
                    }
                    bol = matches!(b, b'\r' | b'\n');
                }
            }
        }
        if !opened && !moved {
            if unsafe { ssh_key(5) } == 3 {
                return Err(Error::msg("cancelled"));
            }
            idle += 1;
            if idle > 6000 {
                return Err(Error::msg("SSH handshake timed out"));
            }
        } else if moved {
            idle = 0;
        }
    }
}
#[unsafe(no_mangle)]
unsafe extern "C" fn ssh_main(args: *const u8, n: usize, transfer: i32) -> i32 {
    // Kernel mailboxes are reused by the client; copy arguments before any I/O.
    let mut saved = [0; 512];
    if n <= saved.len() {
        unsafe {
            core::ptr::copy_nonoverlapping(args, saved.as_mut_ptr(), n);
        }
        let result = match &saved[..n] {
            b"--public-key\0gpu20\0" if transfer == 0 => Some(public_key()),
            b"gpu20\0" => Some(gpu20(transfer != 0)),
            args => Target::parse(args).map(|target| session(target, None, transfer != 0)),
        };
        if let Some(result) = result {
            return match result {
                Ok(code) => code.into(),
                Err(e) => {
                    write!(
                        Console,
                        "{}: {e}\r\n",
                        if transfer != 0 { "sftp" } else { "ssh" }
                    )
                    .unwrap();
                    255
                }
            };
        }
    }
    if transfer != 0 {
        output(b"usage: sftp [-P PORT] user@IPv4 | sftp gpu20\r\n");
        return 2;
    }
    output(b"usage: ssh [-p PORT] user@IPv4 | ssh gpu20 | ssh --public-key gpu20\r\n");
    2
}
