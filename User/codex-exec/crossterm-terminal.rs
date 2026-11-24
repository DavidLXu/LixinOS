unsafe extern "C" {
    fn lx_geometry() -> u64;
    fn lx_codex_terminal() -> i32;
}
static RAW: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
pub(crate) fn is_raw_mode_enabled() -> bool { RAW.load(std::sync::atomic::Ordering::Relaxed) }
pub(crate) fn enable_raw_mode() -> io::Result<()> {
    if unsafe { lx_codex_terminal() } != 1 {
        return Err(io::Error::other("Codex TUI needs a foreground terminal"));
    }
    RAW.store(true, std::sync::atomic::Ordering::Relaxed);
    Ok(())
}
pub(crate) fn disable_raw_mode() -> io::Result<()> {
    RAW.store(false, std::sync::atomic::Ordering::Relaxed);
    Ok(())
}
pub(crate) fn window_size() -> io::Result<WindowSize> {
    let geometry = unsafe { lx_geometry() };
    let columns = geometry as u16;
    let rows = (geometry >> 16) as u16;
    let (columns, rows) = if columns == 0 || rows == 0 { (80, 24) } else { (columns, rows) };
    Ok(WindowSize { columns, rows, width: 0, height: 0 })
}
