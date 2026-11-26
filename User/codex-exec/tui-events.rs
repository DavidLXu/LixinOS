// The guest console wakes immediately from lx_timed_byte on a dedicated kernel
// thread, eliminating the 16ms polling jitter and idle Tokio wakeups.
pub struct CrosstermEventSource {
    rx: tokio::sync::mpsc::UnboundedReceiver<EventResult>,
    stop: std::sync::Arc<std::sync::atomic::AtomicBool>,
    reader: Option<std::thread::JoinHandle<()>>,
}

impl Default for CrosstermEventSource {
    fn default() -> Self {
        let (tx, rx) = tokio::sync::mpsc::unbounded_channel();
        let stop = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
        let stop_flag = std::sync::Arc::clone(&stop);
        let reader = std::thread::Builder::new()
            .name("codex-tui-input".to_string())
            .spawn(move || {
                while !stop_flag.load(std::sync::atomic::Ordering::Acquire) {
                    match crossterm::event::poll(std::time::Duration::from_millis(25)) {
                        Ok(true) => {
                            if tx.send(crossterm::event::read()).is_err() {
                                break;
                            }
                        }
                        Ok(false) => {}
                        Err(err) if err.kind() == std::io::ErrorKind::Interrupted => continue,
                        Err(err) => {
                            let _ = tx.send(Err(err));
                            break;
                        }
                    }
                }
            })
            .ok();
        Self { rx, stop, reader }
    }
}

impl Drop for CrosstermEventSource {
    fn drop(&mut self) {
        self.stop.store(true, std::sync::atomic::Ordering::Release);
        if let Some(handle) = self.reader.take() {
            let _ = handle.join();
        }
    }
}

impl EventSource for CrosstermEventSource {
    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<EventResult>> {
        self.rx.poll_recv(cx)
    }
}
