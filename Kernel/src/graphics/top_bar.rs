//! Pinned text chrome lives above the terminal's scrollable cells.
use super::*;
use alloc::string::String;

const HEIGHT: usize = 2;
const TITLE_BG: u32 = 0x273038;
const CONTEXT_BG: u32 = 0x20272e;

pub(crate) struct TopBar {
    pub title: &'static str,
    pub short_title: &'static str,
    pub context: String,
    pub badge: &'static str,
    pub badge_background: u32,
    pub status: &'static str,
    pub detail: String,
}

pub(crate) struct PinnedBar(Option<TopBar>);

fn with_terminal<T>(f: impl FnOnce(&mut Terminal, &mut [u32], usize) -> T) -> T {
    if popup::task_has_hidden_tab() {
        return popup::with_hidden_tab(crate::scheduler::terminal_id(), f).unwrap();
    }
    let _pointer = suspend_text_pointer();
    if let Some(window) = popup::current() {
        return window.with_terminal(f);
    }
    unsafe {
        let pane = ACTIVE;
        let size = display::size();
        let left = pane * (size.cols + 1) * CW;
        let fb = &mut (*FB).0;
        f(&mut TERMS[pane], &mut fb[BAR_PIXELS + left..], size.terminal_width(pane))
    }
}

pub(crate) fn pin_bar(bar: TopBar) -> Option<PinnedBar> {
    with_terminal(|term, fb, width| {
        (term.rows + term.bar_rows() >= HEIGHT + 2)
            .then(|| PinnedBar(term.replace_bar(fb, width, Some(bar))))
    })
}

impl Drop for PinnedBar {
    fn drop(&mut self) {
        with_terminal(|term, fb, width| { term.replace_bar(fb, width, self.0.take()); });
    }
}

pub(crate) fn update_bar(update: impl FnOnce(&mut TopBar)) -> bool {
    with_terminal(|term, fb, width| {
        let Some(bar) = &mut term.bar else { return false; };
        update(bar);
        term.paint_bar(fb, width);
        true
    })
}

impl Terminal {
    pub(super) fn bar_rows(&self) -> usize {
        if self.bar.is_some() { HEIGHT } else { 0 }
    }

    fn replace_bar(&mut self, fb: &mut [u32], width: usize, bar: Option<TopBar>) -> Option<TopBar> {
        let old_rows = self.bar_rows();
        let previous = core::mem::replace(&mut self.bar, bar);
        let new_rows = self.bar_rows();
        if old_rows != new_rows {
            let body = &mut fb[old_rows * CH * self.stride..];
            self.clear_selection(body);
            if self.history.offset == 0 {
                self.cursor(body);
            } else {
                self.viewport(body, 0);
            }
            let screen = self.capture(body);
            self.rows = self.rows + old_rows - new_rows;
            let body = &mut fb[new_rows * CH * self.stride..];
            self.restore(body, screen);
            self.cursor(body);
        }
        if new_rows == 0 {
            for y in 0..old_rows * CH {
                fb[y * self.stride + self.cols * CW..y * self.stride + width].fill(BG);
            }
        } else {
            self.paint_bar(fb, width);
        }
        previous
    }

    pub(super) fn paint_bar(&self, fb: &mut [u32], width: usize) {
        let Some(bar) = &self.bar else { return; };
        for y in 0..CH {
            fb[y * self.stride..y * self.stride + width].fill(TITLE_BG);
            fb[(y + CH) * self.stride..(y + CH) * self.stride + width].fill(CONTEXT_BG);
        }
        let badge = bar.badge.trim();
        let padded_badge = self.cols >= badge.len() + 2;
        let badge = if padded_badge { bar.badge } else { badge };
        let badge_start = self.cols.saturating_sub(badge.len());
        for title in [bar.title, bar.short_title] {
            if title.len() < badge_start {
                text(fb, self.stride, 0, 0, title, (0xa5edeb, TITLE_BG));
                break;
            }
        }
        text(fb, self.stride, 0, badge_start, &badge[..badge.len().min(self.cols)], (BG, bar.badge_background));
        for y in 0..CH {
            fb[y * self.stride + self.cols * CW..y * self.stride + width].fill(bar.badge_background);
        }

        let detail = &bar.detail;
        let status = bar.status;
        let show_detail = !detail.is_empty() && self.cols >= status.len() + detail.len() + 9;
        let right = if show_detail { alloc::format!("{status}  |  {detail} ") } else { alloc::format!("{status} ") };
        let right = &right[..right.len().min(self.cols)];
        let right_start = self.cols - right.len();
        let path_width = right_start.saturating_sub(2);
        let context = bar.context.as_bytes();
        if context.len() <= path_width {
            text(fb, self.stride, 1, 1, &bar.context, (FG, CONTEXT_BG));
        } else if path_width != 0 {
            let dots = path_width.min(3);
            let tail = path_width - dots;
            text(fb, self.stride, 1, 1, &"..."[..dots], (FG, CONTEXT_BG));
            text(fb, self.stride, 1, 1 + dots, &bar.context[context.len() - tail..], (FG, CONTEXT_BG));
        }
        text(fb, self.stride, 1, right_start, right, (0xb3cfff, CONTEXT_BG));
    }
}

fn text(fb: &mut [u32], stride: usize, row: usize, col: usize, text: &str, (fg, bg): (u32, u32)) {
    for (i, byte) in text.bytes().enumerate() {
        glyph_at(fb, stride, row * CH * stride + (col + i) * CW, CW, byte, fg, bg);
    }
}
