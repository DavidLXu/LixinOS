//! Run desktop child applications between yields from the foreground canvas.
use super::*;
use crate::graphics::{PopupScreen, hide_popup, show_popup};

type Sessions = Vec<(Parent, PopupScreen)>;

pub(super) fn resume(host: &Context, image: &[u8], mut io: utility::Context) -> Result<u8> {
    let mut sessions = Sessions::new();
    loop {
        let canvas = io.canvas.as_ref().ok_or("desktop has no display")?;
        let mail = unsafe { core::slice::from_raw_parts(user::git_mailbox(), 522) };
        let action = i32::from_le_bytes(mail[..4].try_into().unwrap());
        let path = String::from(
            mail[8..]
                .split(|b| *b == 0)
                .next()
                .and_then(|b| core::str::from_utf8(b).ok())
                .unwrap_or(""),
        );
        let mut memory = user::suspend_process();
        canvas.arena.map_canvas(false);
        let result = open(host, action, &path, &mut sessions);
        canvas.arena.map_canvas(true);
        let reply = user::saved_mailbox(&mut memory);
        reply[..4].copy_from_slice(&(sessions.len() as i32).to_le_bytes());
        let error = result.err().unwrap_or("").as_bytes();
        reply[8..8 + error.len()].copy_from_slice(error);
        reply[8 + error.len()] = 0;
        let (returned, status) = io.enter(|| user::resume_agent(image, &memory));
        io = returned;
        if status != Ok(256) {
            return status.and_then(|n| u8::try_from(n).map_err(|_| "desktop fault"));
        }
    }
}
fn open(host: &Context, action: i32, path: &str, sessions: &mut Sessions) -> Result<()> {
    if action == -1 {
        let path = host.services.path(path)?;
        show_popup(None);
        let result = crate::foreground::run(
            "nano",
            Some(path),
            host.services.volume.clone(),
            host.services.started,
        );
        hide_popup(false);
        return result;
    }
    let index = if action == 0 {
        sessions.len()
    } else {
        usize::try_from(action.saturating_sub(1))
            .ok()
            .filter(|i| *i < sessions.len())
            .ok_or("unknown terminal")?
    };
    let (ctx, shell, saved, screen) = if action == 0 {
        // Keep headroom for a foreground compiler and file operations.
        if crate::monitor::heap_usage().1 < 20 * 1024 * 1024 {
            return Err("not enough memory for another terminal");
        }
        let shell = host.services.executable_bytes("/bin/bash")?;
        if !user::is_executable(&shell) {
            return Err("expected /bin/bash in LXE format");
        }
        let mut ctx = Context::new(host.services.clone(), host.history.clone(), false);
        let mut sio = utility::Context::new(
            &ctx.services.cwd,
            ctx.services.volume.clone(),
            utility::Streams::default(),
        );
        sio.popup_shell = true;
        ctx.io = Some(sio);
        (ctx, shell, None, None)
    } else {
        let (state, screen) = sessions.remove(index);
        (state.ctx, state.image, Some(state.memory), Some(screen))
    };
    show_popup(screen);
    let mut suspended = None;
    let result = drive(ctx, shell, saved, true, &mut suspended);
    if let Some(screen) = hide_popup(suspended.is_some()) {
        sessions.insert(index, (suspended.unwrap(), screen));
    }
    result.map(|_| ())
}
