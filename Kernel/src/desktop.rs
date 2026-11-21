//! Run desktop child applications between yields from the foreground canvas.
use super::*;
use crate::graphics::{self, TabAction, hide_popup, show_popup};

struct Tab {
    id: usize,
    pid: usize,
}
type Sessions = Vec<Tab>;

pub(super) fn resume(host: &Context, _image: &[u8], mut io: utility::Context) -> Result<u8> {
    let mut sessions = Sessions::new();
    loop {
        let canvas = io.canvas.as_ref().ok_or("desktop has no display")?;
        let mail = unsafe { core::slice::from_raw_parts(user::git_mailbox(), 522) };
        let action = i32::from_le_bytes(mail[..4].try_into().unwrap());
        let width = u32::from_le_bytes(mail[4..8].try_into().unwrap()) as usize;
        let title = if matches!(action, -2 | -3) { Vec::new() } else { crate::graphics::title_strip(canvas, width)? };
        let path = String::from(
            mail[8..]
                .split(|b| *b == 0)
                .next()
                .and_then(|b| core::str::from_utf8(b).ok())
                .unwrap_or(""),
        );
        let mut memory = user::suspend_process();
        canvas.arena.set_mapped(false);
        let result = open(host, action, &path, &mut sessions, &title);
        canvas.arena.set_mapped(true);
        if matches!(action, -2 | -3) { crate::graphics::begin_canvas(); }
        let reply = user::saved_mailbox(&mut memory);
        reply[..4].copy_from_slice(&(sessions.len() as i32).to_le_bytes());
        let error = result.err().unwrap_or("").as_bytes();
        reply[8..8 + error.len()].copy_from_slice(error);
        reply[8 + error.len()] = 0;
        let (returned, status) = io.enter(|| user::resume_agent(memory));
        io = returned;
        if status != Ok(256) {
            for tab in sessions {
                crate::scheduler::stop_child(tab.pid);
                let _ = crate::scheduler::wait(tab.pid);
                graphics::forget_tab(tab.id);
            }
            return status.and_then(|n| u8::try_from(n).map_err(|_| "desktop fault"));
        }
    }
}
fn open(
    host: &Context,
    action: i32,
    path: &str,
    sessions: &mut Sessions,
    title: &[u32],
) -> Result<()> {
    if action == -2 && path == "/bin/links" {
        let image = host.services.executable_bytes(path)?;
        let code = utility::segment(&image).ok_or("invalid browser executable")?;
        let mut io = utility::Context::new(&host.services.cwd, host.services.volume.clone(), utility::Streams::default());
        io.allow_canvas = true;
        let (_, status) = io.enter(|| user::launch_status(code, Some((&host.services.cwd, "-g\0")), false));
        return if status? == 0 { Ok(()) } else { Err("browser exited with an error") };
    }
    if matches!(action, -2 | -3) {
        let path = host.services.path(path)?;
        let executable = if action == -3 { "/bin/text-editor" } else { &path };
        let image = host.services.executable_bytes(executable)?;
        if !user::is_executable(&image) {
            return Err("expected an LXE1 desktop app");
        }
        let mut io = utility::Context::new(
            &host.services.cwd, host.services.volume.clone(), utility::Streams::default());
        io.allow_canvas = true;
        let mut packed = String::from(executable);
        packed.push('\0');
        let argc = if action == -3 { packed.push_str(&path); packed.push('\0'); 2 } else { 1 };
        let (_, result) = io.enter(|| user::start_saved(&image, argc, &packed, 0));
        return match result? {
            0 => Ok(()),
            _ => Err("desktop app exited with an error"),
        };
    }
    if action == -1 {
        let path = host.services.path(path)?;
        show_popup(None, title, true);
        let result = crate::foreground::run(
            "nano",
            Some(path),
            host.services.volume.clone(),
            host.services.started,
        );
        hide_popup(false);
        return result;
    }
    reap_hidden(sessions);
    let active = if action == 0 {
        start_tab(host, sessions, title, None)?
    } else {
        let index = (action as usize).wrapping_sub(1).min(sessions.len().saturating_sub(1));
        let tab = sessions.get(index).ok_or("unknown terminal")?;
        graphics::show_tab(tab.id, None);
        index
    };
    run_tabs(host, sessions, title, active)
}

fn reap_hidden(sessions: &mut Sessions) {
    sessions.retain(|tab| {
        if crate::scheduler::try_wait(tab.pid).is_none() { return true; }
        graphics::forget_tab(tab.id);
        false
    });
}

fn start_tab(host: &Context, sessions: &mut Sessions, title: &[u32], position: Option<(usize, usize)>) -> Result<usize> {
    if sessions.len() >= graphics::MAX_TABS {
        return Err("terminal tab limit reached");
    }
    if crate::monitor::heap_usage().1 < 20 * 1024 * 1024 {
        return Err("not enough memory for another terminal");
    }
    let shell = host.services.executable_bytes("/bin/bash")?;
    if !user::is_executable(&shell) { return Err("expected /bin/bash in LXE format"); }
    let mut ctx = Context::new(host.services.clone(), host.history.clone(), false);
    let mut io = utility::Context::new(&ctx.services.cwd, ctx.services.volume.clone(), utility::Streams::default());
    io.popup_shell = true;
    ctx.io = Some(io);
    show_popup(None, title, false);
    let id = graphics::terminal_id();
    if position.is_some() {
        graphics::stash_tab();
        graphics::show_tab(id, position);
    }
    let result = crate::scheduler::spawn(alloc::boxed::Box::new(move || {
        drive(ctx, shell, true, None)
    }), crate::scheduler::pane());
    match result {
        Ok(pid) => {
            let index = sessions.len();
            sessions.push(Tab { id, pid });
            Ok(index)
        }
        Err(error) => { hide_popup(false); Err(error) }
    }
}

fn run_tabs(host: &Context, sessions: &mut Sessions, title: &[u32], mut active: usize) -> Result<()> {
    graphics::set_tab_bar(active, sessions.len());
    loop {
        graphics::poll_window();
        let selected_id = sessions[active].id;
        let action = graphics::tab_action();
        let status = crate::scheduler::try_wait(sessions[active].pid);
        if status.is_some() || matches!(action, Some(TabAction::Close)) {
            let tab = sessions.remove(active);
            if status.is_none() {
                crate::scheduler::stop_child(tab.pid);
                let _ = crate::scheduler::wait(tab.pid);
            }
            let position = graphics::stash_tab();
            graphics::forget_tab(tab.id);
            reap_hidden(sessions);
            if sessions.is_empty() { return Ok(()); }
            active = active.min(sessions.len() - 1);
            graphics::show_tab(sessions[active].id, position);
            graphics::set_tab_bar(active, sessions.len());
            continue;
        }
        match action {
            Some(TabAction::Hide) => {
                graphics::stash_tab();
                return Ok(());
            }
            Some(TabAction::Select(index)) if index != active && index < sessions.len() => {
                let position = graphics::stash_tab();
                active = index;
                graphics::show_tab(sessions[active].id, position);
                graphics::set_tab_bar(active, sessions.len());
            }
            Some(TabAction::New) => {
                let position = graphics::stash_tab();
                match start_tab(host, sessions, title, position) {
                    Ok(index) => active = index,
                    Err(_) => graphics::show_tab(selected_id, position),
                }
                graphics::set_tab_bar(active, sessions.len());
            }
            _ => {
                let count = sessions.len();
                sessions.retain(|tab| {
                    if tab.id == selected_id || crate::scheduler::try_wait(tab.pid).is_none() { return true; }
                    graphics::forget_tab(tab.id);
                    false
                });
                if sessions.len() != count {
                    active = sessions.iter().position(|tab| tab.id == selected_id).unwrap();
                    graphics::set_tab_bar(active, sessions.len());
                }
            }
        }
        crate::scheduler::sleep_ms(4);
    }
}
