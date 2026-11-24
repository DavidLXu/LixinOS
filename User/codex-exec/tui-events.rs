// The guest console has a byte queue instead of a SIGWINCH/epoll input descriptor.
// Drive Crossterm's shared decoder from the TUI runtime so pausing drops the reader.
pub struct CrosstermEventSource {
    tick: tokio::time::Interval,
}

impl Default for CrosstermEventSource {
    fn default() -> Self {
        let mut tick = tokio::time::interval(std::time::Duration::from_millis(16));
        tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
        Self { tick }
    }
}

impl EventSource for CrosstermEventSource {
    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<EventResult>> {
        loop {
            match crossterm::event::poll(std::time::Duration::ZERO) {
                Ok(true) => return Poll::Ready(Some(crossterm::event::read())),
                Err(error) => return Poll::Ready(Some(Err(error))),
                Ok(false) => {}
            }
            if self.tick.poll_tick(cx).is_pending() {
                return Poll::Pending;
            }
        }
    }
}
