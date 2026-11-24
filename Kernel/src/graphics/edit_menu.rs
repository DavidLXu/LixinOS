//! Framebuffer overlay shared by terminal windows and application canvases.
use super::*;

pub(super) const RIGHT_PRESS: u64 = 1 << 29;
pub(super) const COPY_EVENT: u64 = 1 << 30;
const LEFT: u64 = 1 << 24;
const INSIDE: u64 = 1 << 25;
const WHEEL: u64 = 3 << 26;
const WIDTH: usize = 9 * CW;
const HEIGHT: usize = 2 * CH + 4;

#[derive(Clone, Copy)]
pub(super) enum Action { Copy, Paste }

struct Menu {
    x: usize,
    y: usize,
    hovered: Option<usize>,
    enabled: bool,
    drawn: bool,
}
static mut MENU: Option<Menu> = None;
static mut UNDERLAY: [u32; WIDTH * HEIGHT] = [0; WIDTH * HEIGHT];

pub(super) fn active() -> bool {
    unsafe { (*addr_of!(MENU)).is_some() }
}

pub(super) fn hide() -> bool {
    let Some(menu) = (unsafe { (*addr_of_mut!(MENU)).as_mut() }) else { return false; };
    if !menu.drawn { return false; }
    display::damage(menu.x, menu.y, WIDTH, HEIGHT);
    let fb = unsafe { &mut (*FB).0 };
    for (row, saved) in unsafe { &*addr_of!(UNDERLAY) }.chunks_exact(WIDTH).enumerate() {
        let start = (menu.y + row) * W + menu.x;
        fb[start..start + WIDTH].copy_from_slice(saved);
    }
    menu.drawn = false;
    true
}

pub(super) fn show() {
    let Some(menu) = (unsafe { (*addr_of_mut!(MENU)).as_mut() }) else { return; };
    if menu.drawn { return; }
    display::damage(menu.x, menu.y, WIDTH, HEIGHT);
    let fb = unsafe { &mut (*FB).0 };
    let start = menu.y * W + menu.x;
    for (row, saved) in unsafe { &mut *addr_of_mut!(UNDERLAY) }.chunks_exact_mut(WIDTH).enumerate() {
        let offset = start + row * W;
        let pixels = &mut fb[offset..offset + WIDTH];
        saved.copy_from_slice(pixels);
        let item = row.saturating_sub(2) / CH;
        let bg = if menu.hovered == Some(item) { 0x395772 } else { 0x25313e };
        pixels.fill(bg);
        pixels[0] = 0x71879b;
        pixels[WIDTH - 1] = 0x71879b;
        if !(2..HEIGHT - 2).contains(&row) { pixels.fill(0x71879b); }
    }
    for (item, label) in ["Copy", "Paste"].into_iter().enumerate() {
        let enabled = item != 0 || menu.enabled;
        let ink = if enabled { 0xf0f5fc } else { 0x8494a4 };
        let bg = if menu.hovered == Some(item) { 0x395772 } else { 0x25313e };
        for (col, byte) in label.bytes().enumerate() {
            glyph(fb, start + (2 + item * CH) * W + (col + 1) * CW, CW, byte, ink, bg);
        }
    }
    menu.drawn = true;
}

pub(super) fn close() {
    hide();
    unsafe { *addr_of_mut!(MENU) = None; }
}

// The caller lifts the pointer first. Preserve coordinates for the application
// while feeding it an outside event so it can clear hover and end a grab.
pub(super) fn input(event: u64, copy_enabled: impl FnOnce() -> bool) -> (u64, Option<Action>) {
    let x = (event & 4095) as usize * 2 + unsafe { ACTIVE } * (pane_width() + CW);
    let y = ((event >> 12) & 4095) as usize * 2 + CH;
    if event & INSIDE != 0 && event & RIGHT_PRESS != 0 {
        close();
        let left = unsafe { ACTIVE } * (pane_width() + CW);
        unsafe {
            *addr_of_mut!(MENU) = Some(Menu {
                x: x.min(left + pane_width() - WIDTH),
                y: y.min(CH + 2 * canvas_h() - HEIGHT),
                hovered: None,
                enabled: copy_enabled(),
                drawn: false,
            });
        }
    }
    let Some(menu) = (unsafe { (*addr_of_mut!(MENU)).as_mut() }) else {
        return (event & !RIGHT_PRESS, None);
    };
    let hit = (event & INSIDE != 0 && (menu.x..menu.x + WIDTH).contains(&x)
        && (menu.y + 2..menu.y + HEIGHT - 2).contains(&y))
        .then(|| (y - menu.y - 2) / CH);
    let action = if event & LEFT != 0 {
        let action = match hit {
            Some(0) if menu.enabled => Some(Action::Copy),
            Some(1) => Some(Action::Paste),
            _ => None,
        };
        close();
        suppress_left();
        action
    } else {
        if menu.hovered != hit {
            hide();
            if let Some(menu) = unsafe { (*addr_of_mut!(MENU)).as_mut() } { menu.hovered = hit; }
        }
        show();
        None
    };
    (event & !(LEFT | INSIDE | WHEEL | RIGHT_PRESS), action)
}
