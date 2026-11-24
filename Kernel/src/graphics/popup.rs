//! One foreground text window per pane. Its backing buffer holds the desktop
//! pixels directly underneath it. Dragging moves live pixels in place.
use super::*;
use alloc::vec::Vec;
const BORDER: usize = 6;
const TITLE: usize = 44;
const TAB_LEFT: usize = 164;
const TAB_WIDTH: usize = 30;
pub const MAX_TABS: usize = 4;

#[derive(Clone, Copy)]
pub enum TabAction { New, Select(usize), Close, Hide }

struct TabBar {
    count: usize,
    action: Option<TabAction>,
}
static mut TAB_BARS: [Option<TabBar>; PANES] = [const { None }; PANES];
static mut HIDDEN_TABS: [Vec<PopupScreen>; PANES] = [const { Vec::new() }; PANES];

fn tab_bar() -> &'static mut Option<TabBar> {
    unsafe { &mut (*addr_of_mut!(TAB_BARS))[ACTIVE] }
}

fn hidden_tabs() -> &'static mut Vec<PopupScreen> {
    unsafe { &mut (*addr_of_mut!(HIDDEN_TABS))[ACTIVE] }
}

pub fn active_tab() -> Option<usize> { tab_bar().as_ref().map(|_| terminal_id()) }

pub fn tab_input_allowed() -> bool {
    let id = crate::scheduler::terminal_id();
    if let Some(visible) = active_tab() { return id == visible; }
    !hidden_tabs().iter().any(|screen| screen.3 == id)
}

pub(super) fn task_has_hidden_tab() -> bool {
    let id = crate::scheduler::terminal_id();
    id != 0 && hidden_tabs().iter().any(|screen| screen.3 == id)
}

pub(super) fn with_hidden_tab<T>(id: usize, f: impl FnOnce(&mut Terminal, &mut [u32], usize) -> T) -> Option<T> {
    let PopupScreen(_, term, pixels, _) = hidden_tabs().iter_mut().find(|screen| screen.3 == id)?;
    let start = 2 * (TITLE * term.stride + BORDER);
    Some(f(term, &mut pixels[start..], term.cols * CW))
}

pub(super) fn write_hidden_tab(id: usize, byte: u8) -> bool {
    with_hidden_tab(id, |term, pixels, _| {
        let start = term.bar_rows() * CH * term.stride;
        term.byte(&mut pixels[start..], byte);
    }).is_some()
}

pub fn stash_tab() -> Option<(usize, usize)> {
    *tab_bar() = None;
    let mut screen = hide_popup(true)?;
    let position = screen.0;
    screen.1.stride = 2 * popup_width();
    hidden_tabs().push(screen);
    Some(position)
}

pub fn show_tab(id: usize, position: Option<(usize, usize)>) {
    let tabs = hidden_tabs();
    let Some(index) = tabs.iter().position(|tab| tab.3 == id) else { return };
    let mut screen = tabs.swap_remove(index);
    if let Some(pos) = position { screen.0 = pos; }
    screen.1.stride = W;
    show_popup(Some(screen), &[], false);
}

pub fn forget_tab(id: usize) {
    hidden_tabs().retain(|screen| screen.3 != id);
}

pub fn set_tab_bar(active: usize, count: usize) {
    *tab_bar() = Some(TabBar { count, action: None });
    let _overlay = suspend_text_pointer();
    let Some(win) = current() else { return };
    let fb = framebuffer();
    for index in 0..=MAX_TABS {
        let x = win.position.0 + TAB_LEFT + index * TAB_WIDTH;
        let y = win.position.1 + 8;
        let start = offset((x, y));
        let selected = index == active;
        let exists = index < count || (index == count && count < MAX_TABS);
        let bg = if selected { 0x395772 } else if exists { 0x202e38 } else { 0xe8eee8 };
        for row in 0..56 {
            let at = start + row * W;
            fb[at..at + 2 * (TAB_WIDTH - 4)].fill(bg);
        }
        if exists {
            let label = if index < count { b'1' + index as u8 } else { b'+' };
            glyph(fb, offset((x + 8, y + 3)), CW, label, 0xf0f5fc, bg);
        }
    }
}

pub fn tab_action() -> Option<TabAction> {
    let action = tab_bar().as_mut().and_then(|bar| bar.action.take());
    let win = current()?;
    if core::mem::take(&mut win.close) { Some(TabAction::Close) }
    else if core::mem::take(&mut win.minimize) { Some(TabAction::Hide) }
    else { action }
}

fn tab_click(x: usize, y: usize) -> bool {
    let Some(bar) = tab_bar().as_mut() else { return false; };
    if !(8..36).contains(&y) || x < TAB_LEFT { return false; }
    let tab = (x - TAB_LEFT) / TAB_WIDTH;
    if tab < bar.count { bar.action = Some(TabAction::Select(tab)); }
    else if tab == bar.count && tab < MAX_TABS { bar.action = Some(TabAction::New); }
    else { return false; }
    true
}
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
    down: bool,
    grab: Option<Point>,
    close: bool,
    minimize: bool,
    paste: bool,
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
    display::damage(0, CH, W, H);
    unsafe { &mut (*FB).0 }
}

pub fn popup_active() -> bool {
    current().is_some()
}
pub fn columns() -> usize {
    if task_has_hidden_tab() || popup_active() { popup_cols() } else { display::size().cols }
}
pub fn rows() -> usize {
    if let Some(rows) = with_hidden_tab(crate::scheduler::terminal_id(), |term, _, _| term.rows) {
        return rows;
    }
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
        if event & (1 << 28) != 0 {
            self.down = false;
            self.grab = None;
            self.selecting = false;
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
                } else if !tab_click(dx, dy) {
                    self.grab = Some((dx, dy));
                }
            }
        }
        let (left, top) = (self.position.0 + BORDER, self.position.1 + TITLE);
        let point = (inside && (left..left + popup_cols() * CW / 2).contains(&x)
            && (top..top + popup_rows() * CH / 2).contains(&y))
            .then(|| ((x - left) * 2 / CW, (y - top) * 2 / CH));
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
    }
}
pub fn show_popup(saved: Option<PopupScreen>, title: &[u32], editor: bool) {
    hide_text_pointer();
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
        down: false,
        grab: None,
        close: false,
        minimize: false,
        paste: false,
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
    hide_text_pointer();
    edit_menu::close();
    let start = win.text_origin() + win.term.bar_rows() * BAR_PIXELS;
    win.term.clear_selection(&mut framebuffer()[start..]);
    win.swap_pixels();
    save.then(|| PopupScreen(win.position, win.term, win.underlay,win.id))
}
pub(super) fn write_popup(byte: u8) -> bool {
    let Some(win) = current() else {
        return false;
    };
    let start = win.text_origin() + win.term.bar_rows() * BAR_PIXELS;
    win.term.visible_byte(start, byte);
    true
}
pub fn poll_window() {
    if !pointer_focused() || task_has_hidden_tab() {
        return;
    }
    if crate::foreground::canvas_active() || (!popup_active() && unsafe { CANVAS_OPEN[ACTIVE] }) {
        return;
    }
    loop {
        let event = read_pointer();
        if event == u64::MAX { break; }
        hide_text_pointer();
        let (event, action) = edit_menu::input(event, || editor_popup() || !copied_text().is_empty());
        if let Some(win) = current() {
            win.input(event);
        } else {
            console_selection(event);
        }
        match action {
            Some(edit_menu::Action::Copy) if editor_popup() => {
                if let Some(win) = current() { win.pointer_events.push_back(3); }
            }
            Some(edit_menu::Action::Copy) => crate::clipboard::copy(copied_text()),
            Some(edit_menu::Action::Paste) if active_tab().is_some() => {
                if let Some(win) = current() { win.paste = true; }
            }
            Some(edit_menu::Action::Paste) => crate::clipboard::paste(),
            None => (),
        }
    }
    show_text_pointer();
}
pub fn tab_paste() {
    if active_tab().is_some() && current().is_some_and(|win| core::mem::take(&mut win.paste)) {
        crate::clipboard::paste();
    }
}

pub(super) fn over_text(x: usize, y: usize) -> bool {
    if let Some(win) = current() {
        let left = win.position.0 + BORDER;
        let top = win.position.1 + TITLE + win.term.bar_rows() * CH / 2;
        return (left..left + win.term.cols * CW / 2).contains(&x)
            && (top..top + win.term.rows * CH / 2).contains(&y);
    }
    y >= unsafe { TERMS[ACTIVE].bar_rows() } * CH / 2
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
        poll_window();
        if let Some(win) = current() {
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
    active_tab().is_none() && current().is_some_and(|w| !w.editor && w.close)
}
pub fn window_input() -> Option<u8> {
    let w = current()?;
    if w.close && active_tab().is_none() {
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
        if crate::scheduler::stopped() != 0 { return 4; }
        if let Some(c) = crate::poll_byte() {
            if c != 29 { return u64::from(c); }
            if active_tab().is_none() { return 257; }
            if let Some(w) = current() { w.minimize = true; }
        }
        crate::scheduler::sleep_ms(1);
    }
}
