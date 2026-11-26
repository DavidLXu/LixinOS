// Reuse the upstream byte parser, including bracketed paste and protected-input quarantine.
unsafe extern "C" {
    fn lx_timed_byte(ms: u32) -> i32;
}

pub(crate) struct UnixInternalEventSource {
    parser: Parser,
    size: (u16, u16),
    #[cfg(feature = "event-stream")]
    waker: Waker,
}

impl UnixInternalEventSource {
    pub fn new() -> io::Result<Self> {
        Ok(Self {
            parser: Parser::default(),
            size: crate::terminal::size()?,
            #[cfg(feature = "event-stream")]
            waker: Waker::default(),
        })
    }
}

impl EventSource for UnixInternalEventSource {
    fn try_read(&mut self, timeout: Option<Duration>) -> io::Result<Option<InternalEvent>> {
        let deadline = PollTimeout::new(timeout);
        loop {
            if let Some(event) = self.parser.next() {
                return Ok(Some(event));
            }
            #[cfg(feature = "event-stream")]
            if self.waker.take() {
                return Err(io::ErrorKind::Interrupted.into());
            }
            let size = crate::terminal::size()?;
            if self.size != size {
                self.size = size;
                return Ok(Some(InternalEvent::Event(Event::Resize(size.0, size.1))));
            }

            let mut input = [0; 256];
            let mut len = 0;
            while len < input.len() {
                let Ok(byte) = u8::try_from(unsafe { lx_timed_byte(0) }) else { break };
                input[len] = byte;
                len += 1;
            }
            if len > 0 {
                self.parser.buffer_external_input(&input[..len]);
                continue;
            }
            if let Some(event) = self.parser.finish_pending_escape() {
                return Ok(Some(event));
            }
            if deadline.elapsed() {
                return Ok(None);
            }
            let wait_ms = match deadline.leftover() {
                Some(rem) => rem.as_millis().clamp(1, 25) as u32,
                None => 25,
            };
            if let Ok(byte) = u8::try_from(unsafe { lx_timed_byte(wait_ms) }) {
                input[0] = byte;
                len = 1;
                while len < input.len() {
                    let Ok(next) = u8::try_from(unsafe { lx_timed_byte(0) }) else { break };
                    input[len] = next;
                    len += 1;
                }
                self.parser.buffer_external_input(&input[..len]);
            }
        }
    }

    fn buffer_input(&mut self, input: &[u8], events: &mut VecDeque<InternalEvent>) {
        self.parser.buffer_external_input(input);
        events.extend(self.parser.by_ref().filter(|event| matches!(event, InternalEvent::Event(_))));
    }

    fn discard_buffered_input(&mut self) -> InputDiscardStatus {
        self.parser.discard_buffered_input()
    }

    #[cfg(feature = "event-stream")]
    fn waker(&self) -> Waker { self.waker.clone() }
}
