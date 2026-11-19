//! One foreground text window per pane. Its backing buffer holds the desktop
//! pixels directly underneath it. Dragging moves live pixels in place.
use super::*;
use alloc::vec::Vec;
const BORDER: usize = 6;
const TITLE: usize = 44;
fn popup_cols() -> usize { (display::size().cols - 8).min(76) }
fn popup_rows() -> usize { ((canvas_h() - TITLE - BORDER) * 2 / CH).min(21) }
fn popup_width() -> usize { popup_cols() * CW / 2 + 2 * BORDER }
fn popup_height() -> usize { popup_rows() * CH / 2 + TITLE + BORDER }
type Point = (usize, usize);

pub(super) struct Window {
    id:usize,
    position: Point,
    underlay: Vec<u32>,
    term: Terminal,
    editor: bool,
    selecting: bool,
    pointer_events: alloc::collections::VecDeque<u32>,
    pointer_position: Option<Point>,
    down: bool,
    grab: Option<Point>,
    cursor: Option<Point>,
    close: bool,
    minimize: bool,
}
static mut WINDOWS: [Option<Window>; PANES] = [const { None }; PANES];
pub struct PopupScreen(Point, Terminal, Vec<u32>, usize);
static mut NEXT_WINDOW:usize=1;
pub fn terminal_id()->usize {current().map_or(0,|w|w.id)}


pub(super) fn current() -> Option<&'static mut Window> {
    unsafe { (&mut (*addr_of_mut!(WINDOWS))[ACTIVE]).as_mut() }
}
fn offset((x, y): Point) -> usize {
    BAR_PIXELS + unsafe { ACTIVE } * (pane_width() + CW) + 2 * (y * W + x)
}
fn framebuffer() -> &'static mut [u32] {
    unsafe { &mut (*addr_of_mut!(FB)).0 }
}

pub fn popup_active() -> bool {
    current().is_some()
}
pub fn columns() -> usize {
    if popup_active() { popup_cols() } else { display::size().cols }
}
pub fn rows() -> usize {
    current().map_or_else(|| unsafe { TERMS[ACTIVE].rows }, |win| win.term.rows)
}

pub fn title_strip(canvas: &Canvas, width: usize) -> Result<Vec<u32>, &'static str> {
    let base = crate::user::ARENA_BASE;
    if width != popup_width() || !canvas.arena.contains(base, canvas_w() * TITLE * 4) {
        return Err("invalid desktop title bar");
    }
    let source = unsafe { core::slice::from_raw_parts(base as *const u32, canvas_w() * TITLE) };
    let mut strip = Vec::with_capacity(popup_width() * TITLE);
    for row in source.chunks_exact(canvas_w()) {
        strip.extend_from_slice(&row[..popup_width()]);
    }
    Ok(strip)
}
impl Window {
    pub(super) fn with_terminal<T>(&mut self, f: impl FnOnce(&mut Terminal, &mut [u32], usize) -> T) -> T {
        self.erase_pointer();
        let start = self.text_origin();
        let width = self.term.cols * CW;
        f(&mut self.term, &mut framebuffer()[start..], width)
    }

    fn text_origin(&self) -> usize {
        let (x, y) = self.position;
        offset((x + BORDER, y + TITLE))
    }

    fn swap_pixels(&mut self) {
        let start = offset(self.position);
        let fb = framebuffer();
        for (y, row) in self.underlay.chunks_exact_mut(2 * popup_width()).enumerate() {
            let begin = start + y * W;
            row.swap_with_slice(&mut fb[begin..begin + 2 * popup_width()]);
        }
    }
    fn erase_pointer(&mut self) {
        if let Some((x, y)) = self.cursor.take() {
            toggle_pointer(x, y);
        }
    }
    fn move_to(&mut self, pos: Point) {
        let old = self.position;
        if pos == old {
            return;
        }
        let width = 2 * popup_width();
        let height = 2 * popup_height();
        let old_start = offset(old);
        let new_start = offset(pos);
        let fb = framebuffer();

        // In each old row the exposed desktop is one strip (or the whole row).
        // Keep just those pixels while shifting the overlapping background.
        let dx = 2 * old.0.abs_diff(pos.0).min(popup_width());
        let dy = 2 * old.1.abs_diff(pos.1).min(popup_height());
        let old_x = if pos.0 > old.0 { dx } else { 0 };
        let old_y = if pos.1 > old.1 { dy } else { 0 };
        let new_x = dx - old_x;
        let new_y = dy - old_y;
        let overlap_w = width - dx;
        let overlap_h = height - dy;
        let strip = |row, x, y| {
            if !(y..y + overlap_h).contains(&row) {
                0..width
            } else if x == 0 {
                overlap_w..width
            } else {
                0..x
            }
        };
        let mut exposed = Vec::with_capacity(width * height - overlap_w * overlap_h);
        for row in 0..height {
            let edge = strip(row, old_x, old_y);
            let start = row * width;
            exposed.extend_from_slice(&self.underlay[start + edge.start..start + edge.end]);
        }
        for i in 0..overlap_h {
            let row = if new_y > old_y { overlap_h - 1 - i } else { i };
            let src = (old_y + row) * width + old_x;
            let dst = (new_y + row) * width + new_x;
            self.underlay.copy_within(src..src + overlap_w, dst);
        }

        // Read the entering edge before drawing over it. Copy the live window
        // in memmove order so even a one-pixel drag preserves every glyph.
        for i in 0..height {
            let row = if pos.1 > old.1 { height - 1 - i } else { i };
            let dst = new_start + row * W;
            let edge = strip(row, new_x, new_y);
            let bg = row * width;
            self.underlay[bg + edge.start..bg + edge.end]
                .copy_from_slice(&fb[dst + edge.start..dst + edge.end]);
            let src = old_start + row * W;
            fb.copy_within(src..src + width, dst);
        }
        let mut saved = exposed.as_slice();
        for row in 0..height {
            let edge = strip(row, old_x, old_y);
            let (pixels, rest) = saved.split_at(edge.len());
            saved = rest;
            let start = old_start + row * W;
            fb[start + edge.start..start + edge.end].copy_from_slice(pixels);
        }
        self.position = pos;
    }
    fn input(&mut self, event: u64) {
        self.erase_pointer();
        if event & (1 << 28) != 0 {
            self.down = false;
            self.grab = None;
            self.selecting = false;
            self.pointer_position = None;
            return;
        }
        let x = (event & 4095) as usize;
        let y = ((event >> 12) & 4095) as usize;
        let down = event & (1 << 24) != 0;
        let inside = event & (1 << 25) != 0;
        if let Some((gx, gy)) = self.grab {
            self.move_to((
                x.saturating_sub(gx).min(canvas_w() - popup_width()),
                y.saturating_sub(gy).min(canvas_h() - popup_height()),
            ));
            if !down {
                self.grab = None;
            }
        } else if inside && down && !self.down {
            let (px, py) = self.position;
            if (px..px + popup_width()).contains(&x) && (py..py + TITLE).contains(&y) {
                let (dx, dy) = (x - px, y - py);
                if (8..36).contains(&dy) && (popup_width() - 35..popup_width() - 7).contains(&dx) {
                    self.close = true;
                } else if !self.editor
                    && (8..36).contains(&dy)
                    && (popup_width() - 68..popup_width() - 40).contains(&dx)
                {
                    self.minimize = true;
                } else {
                    self.grab = Some((dx, dy));
                }
            }
        }
        let (left, top) = (self.position.0 + BORDER, self.position.1 + TITLE);
        let point = (inside && (left..left + popup_cols() * CW / 2).contains(&x)
            && (top..top + popup_rows() * CH / 2).contains(&y))
            .then(|| ((x - left) * 2 / CW, (y - top) * 2 / CH));
        self.pointer_position = inside.then_some((x,y));
        if self.editor {
            let press = down && !self.down && point.is_some();
            if press || self.selecting {
                // Keep press and release boundaries while coalescing motion in one cell grid.
                let col = (x.saturating_sub(left) * 2 / CW).min(popup_cols() - 1);
                let row = (y.saturating_sub(top) * 2 / CH).min(popup_rows() - 1);
                let phase = if press { 1 } else if down { 2 } else { 3 };
                let event = (phase << 20) | ((row as u32) << 8) | col as u32;
                if phase == 2 && self.pointer_events.back().is_some_and(|e| e >> 20 == 2) {
                    self.pointer_events.pop_back();
                }
                self.pointer_events.push_back(event);
                self.selecting = down;
            }
        } else {
            let pixels = &mut framebuffer()[self.text_origin() + self.term.bar_rows() * BAR_PIXELS..];
            if point.is_some() { self.term.wheel(pixels, event); }
            let body_point = point.and_then(|(col, row)| row.checked_sub(self.term.bar_rows()).map(|row| (col, row)));
            self.term.select(pixels, body_point, down);
        }
        self.down = down;
        if inside {
            toggle_pointer(x, y);
            self.cursor = Some((x, y));
        }
    }
}
pub fn show_popup(saved: Option<PopupScreen>, title: &[u32], editor: bool) {
    reset_pointer();
    let mut win = Window {
        id: unsafe { let id=NEXT_WINDOW; NEXT_WINDOW+=1;id },
        position: ((canvas_w() - popup_width()) / 2, (canvas_h() - popup_height()) / 2),
        term: Terminal {
            cols: popup_cols(),
            rows: popup_rows(),
            ..INITIAL
        },
        underlay: Vec::new(),
        editor,
        selecting: false,
        pointer_events: Default::default(),
        pointer_position: None,
        down: false,
        grab: None,
        cursor: None,
        close: false,
        minimize: false,
    };
    if let Some(PopupScreen(pos, term, pixels,id)) = saved {
        win.id=id;
        win.position = pos;
        win.term = term;
        win.underlay = pixels;
    } else {
        win.underlay = alloc::vec![BG;4*popup_width()*popup_height()];
        // Paint the window off-screen, then swap it with the desktop underneath.
        for (y, row) in win
            .underlay
            .chunks_exact_mut(2 * popup_width())
            .take(2 * TITLE)
            .enumerate()
        {
            for (pair, c) in row.chunks_exact_mut(2).zip(&title[(y / 2) * popup_width()..]) {
                pair.fill(*c);
            }
        }
        for y in 2 * TITLE..2 * popup_height() {
            let row = &mut win.underlay[y * 2 * popup_width()..(y + 1) * 2 * popup_width()];
            if y >= 2 * (popup_height() - BORDER) {
                row.fill(0x233c3d);
            } else {
                row[..2 * BORDER].fill(0x233c3d);
                row[2 * (popup_width() - BORDER)..].fill(0x233c3d);
            }
        }
        let start = (2 * TITLE + CH - 3) * 2 * popup_width() + 2 * BORDER;
        for p in &mut win.underlay[start..start + CW] {
            *p ^= 0xb0b0b0;
        }
    }
    win.swap_pixels();
    unsafe {
        WINDOWS[ACTIVE] = Some(win);
    }
}
pub fn hide_popup(save: bool) -> Option<PopupScreen> {
    let mut win = unsafe { WINDOWS[ACTIVE].take()? };
    win.erase_pointer();
    let start = win.text_origin() + win.term.bar_rows() * BAR_PIXELS;
    win.term.clear_selection(&mut framebuffer()[start..]);
    win.swap_pixels();
    save.then(|| PopupScreen(win.position, win.term, win.underlay,win.id))
}
pub(super) fn write_popup(byte: u8) -> bool {
    let Some(win) = current() else {
        return false;
    };
    win.erase_pointer();
    let start = win.text_origin() + win.term.bar_rows() * BAR_PIXELS;
    win.term.byte(&mut framebuffer()[start..], byte);
    true
}
pub fn poll_window() {
    let Some(win) = current() else {
        poll_console_selection();
        return;
    };
    let mut event = pointer_event();
    while event != u64::MAX {
        let mut next = pointer_event();
        // Once grabbed, skip stale motion with the same button/inside state.
        // Deliver presses, releases and cancellations in their original order.
        if win.grab.is_some() && event & (1 << 24) != 0 {
            while next != u64::MAX && next >> 24 == event >> 24 {
                event = next;
                next = pointer_event();
            }
        }
        win.input(event);
        event = next;
    }
}
pub(super) fn popup_selection() -> Option<Vec<u8>> {
    Some(current()?.term.selection_text())
}
pub(super) fn application_cursor(pane:usize) -> Option<bool> {
    unsafe { (*addr_of!(WINDOWS))[pane].as_ref().map(|w|w.term.application_cursor) }
}
pub fn editor_popup() -> bool {
    current().is_some_and(|win| win.editor)
}
pub fn editor_event() -> u64 {
    loop {
        if let Some(win) = current() {
            if win.cursor.is_none() {
                if let Some((x,y)) = win.pointer_position {
                    toggle_pointer(x,y);
                    win.cursor = Some((x,y));
                }
            }
            if let Some(event) = win.pointer_events.pop_front() {
                return u64::from(event);
            }
        }
        if let Some(byte) = crate::poll_byte() {
            return u64::from(byte);
        }
        crate::scheduler::sleep_ms(1);
    }
}
pub fn terminal_closed() -> bool {
    current().is_some_and(|w| !w.editor && w.close)
}
pub fn window_input() -> Option<u8> {
    let w = current()?;
    if w.close {
        if w.editor {
            w.close = false;
            Some(17)
        } else {
            Some(4)
        }
    } else {
        None
    }
}
pub fn popup_key() -> u64 {
    loop {
        if let Some(c) = crate::poll_byte() {
            return if c == 29 { 257 } else { u64::from(c) };
        }
        if current().is_some_and(|w| core::mem::take(&mut w.minimize)) {
            return 257;
        }
        crate::scheduler::sleep_ms(1);
    }
}
