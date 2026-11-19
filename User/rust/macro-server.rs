// Included in the compiler's private copy of proc_macro::bridge::server.
fn remote_expand<S: Server>(
    dispatcher: &mut Dispatcher<S>,
    input: Buffer,
    path: &str,
    index: usize,
    show_panics: bool,
) -> std::io::Result<Buffer> {
    use std::io::{Read, Write};
    use std::process::{Command, Stdio};
    use std::os::fd::AsRawFd;

    // Only the external macro worker can satisfy these operations. Sleeping
    // directly on its kernel pipe avoids a timer tick for each token request.
    struct Pipe<T>(T);
    fn count(n: i32) -> std::io::Result<usize> {
        usize::try_from(n).map_err(|_| std::io::ErrorKind::BrokenPipe.into())
    }
    impl<T: AsRawFd> Read for Pipe<T> {
        fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
            count(super::pipe::read(self.0.as_raw_fd(), buf))
        }
    }
    impl<T: AsRawFd> Write for Pipe<T> {
        fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
            count(super::pipe::write(self.0.as_raw_fd(), buf))
        }
        fn flush(&mut self) -> std::io::Result<()> { Ok(()) }
    }

    fn send(out: &mut impl Write, tag: u8, body: &[u8]) -> std::io::Result<()> {
        out.write_all(&[tag])?;
        out.write_all(&(body.len() as u32).to_le_bytes())?;
        out.write_all(body)
    }

    let mut child = Command::new(path).arg(index.to_string())
        .stdin(Stdio::piped()).stdout(Stdio::piped()).spawn()?;
    let result = (|| {
        let mut requests = Pipe(child.stdout.take().unwrap());
        let mut replies = Pipe(child.stdin.take().unwrap());
        send(&mut replies, u8::from(show_panics), &input)?;
        loop {
            let mut head = [0; 5];
            requests.read_exact(&mut head)?;
            let len = u32::from_le_bytes(head[1..].try_into().unwrap()) as usize;
            if len > 64 << 20 { return Err(std::io::ErrorKind::InvalidData.into()); }
            let mut bytes = vec![0; len];
            requests.read_exact(&mut bytes)?;
            match head[0] {
                1 => send(&mut replies, 0, &dispatcher.dispatch(bytes.into()))?,
                2 => return Ok(bytes.into()),
                _ => return Err(std::io::ErrorKind::InvalidData.into()),
            }
        }
    })();
    let status = child.wait()?;
    if !status.success() { return Err(std::io::Error::other(format!("macro {path} exited with {status}"))); }
    result
}
