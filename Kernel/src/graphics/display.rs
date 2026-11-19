//! Window geometry and fixed-cell terminal reflow.
use super::*;
use alloc::vec::Vec;

#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) struct Size {
    pub width: usize,
    pub height: usize,
    pub cols: usize,
    pub rows: usize,
}

const DEFAULT: Size = Size {
    width: (COLS * PANES + PANES - 1) * CW,
    height: (ROWS + 1) * CH,
    cols: COLS,
    rows: ROWS,
};
static mut SIZE: Size = DEFAULT;
static mut REQUEST: Option<Size> = None;
static mut SELECTOR: u16 = 0;
static mut CLOCK_OFFSET: i32 = 0;

pub(super) fn size() -> Size {
    unsafe { SIZE }
}

pub(super) fn init() {
    let (selector, clock_offset) = display_config();
    unsafe {
        SELECTOR = selector;
        CLOCK_OFFSET = clock_offset;
    }
    configure_ramfb(selector, DEFAULT.width, DEFAULT.height);
}

pub(super) fn clock_offset() -> i32 {
    unsafe { CLOCK_OFFSET }
}

pub(crate) fn request_resize(width: usize, height: usize) {
    // Keep a usable prompt in each pane even if the host is minimized.
    let width = width.clamp((12 * PANES + PANES - 1) * CW, W);
    let height = height.clamp(6 * CH, H);
    let cols = (width / CW - (PANES - 1)) / PANES;
    let rows = height / CH - 1;
    unsafe {
        REQUEST = Some(Size {
            width,
            height,
            cols,
            rows,
        });
    }
}

fn origin(pane: usize, cols: usize) -> usize {
    BAR_PIXELS + pane * (cols + 1) * CW
}

impl Size {
    pub(super) fn terminal_width(self, pane: usize) -> usize {
        if pane + 1 == PANES { self.width - pane * (self.cols + 1) * CW } else { self.cols * CW }
    }
}

pub(crate) fn apply_resize() {
    let Some(next) = (unsafe { REQUEST }) else {
        return;
    };
    if next == size() {
        unsafe {
            REQUEST = None;
        }
        return;
    }
    // Canvases and fixed-size foreground programs draw into their original viewport.
    if unsafe { (*addr_of!(CANVAS_OPEN)).iter().any(|&open| open) }
        || crate::foreground::blocks_resize()
    {
        return;
    }
    unsafe {
        let old = SIZE;
        let fb = &mut (*addr_of_mut!(FB)).0;
        let mut screens = Vec::with_capacity(PANES);
        for pane in 0..PANES {
            let term = &mut TERMS[pane];
            let pixels = &mut fb[origin(pane, old.cols) + term.bar_rows() * BAR_PIXELS..];
            term.clear_selection(pixels);
            if term.history.offset != 0 {
                term.viewport(pixels,0);
            } else {
                term.cursor(pixels);
            }
            screens.push(term.capture(pixels));
        }
        for y in CH..next.height {
            fb[y * W..y * W + next.width].fill(BG);
            if PANES == 2 {
                let divider = y * W + next.cols * CW + CW / 2;
                fb[divider..divider + 2].fill(0x44525b);
            }
        }
        SIZE = next;
        REQUEST = None;
        for (pane, screen) in screens.into_iter().enumerate() {
            let term = &mut TERMS[pane];
            term.cols = next.cols;
            term.rows = next.rows - term.bar_rows();
            let base = &mut fb[origin(pane, next.cols)..];
            term.paint_bar(base, next.terminal_width(pane));
            let pixels = &mut base[term.bar_rows() * BAR_PIXELS..];
            term.restore(pixels, screen);
            term.cursor(pixels);
        }
        reset_pointer();
        draw_status(USAGE.busy_percent);
        configure_ramfb(SELECTOR, next.width, next.height);
    }
}

pub(super) struct Screen {
    pixels: Vec<u32>,
    cells: Vec<u8>,
    styles: Vec<scrollback::Style>,
    wrapped: [bool; MAX_ROWS],
    cols: usize,
    rows: usize,
    cursor: (usize, usize),
    wrap_pending: bool,
}

impl Terminal {
    pub(super) fn capture(&self, fb: &[u32]) -> Screen {
        let width = self.cols * CW;
        let height = self.rows * CH;
        let mut pixels = Vec::with_capacity(width * height);
        for row in 0..height {
            pixels.extend_from_slice(&fb[row * W..row * W + width]);
        }
        Screen {
            pixels,
            styles: self.styles.clone(),
            cells: self.cells[..self.cols * self.rows].to_vec(),
            wrapped: self.wrapped,
            cols: self.cols,
            rows: self.rows,
            cursor: (self.x, self.y),
            wrap_pending: self.wrap_pending,
        }
    }

    pub(super) fn restore(&mut self, fb: &mut [u32], screen: Screen) {
        self.styles.resize(self.cols*self.rows, scrollback::DEFAULT_STYLE);
        self.margins = None;
        self.saved_cursor.0 = self.saved_cursor.0.min(self.cols - 1);
        self.saved_cursor.1 = self.saved_cursor.1.min(self.rows - 1);
        if self.cols == screen.cols && self.rows == screen.rows {
            let width = self.cols * CW;
            for (row, pixels) in screen.pixels.chunks_exact(width).enumerate() {
                fb[row * W..row * W + width].copy_from_slice(pixels);
            }
            self.cells[..screen.cells.len()].copy_from_slice(&screen.cells);
            self.styles[..screen.styles.len()].copy_from_slice(&screen.styles);
            self.wrapped = screen.wrapped;
            (self.x, self.y) = screen.cursor;
            self.wrap_pending = screen.wrap_pending;
            return;
        }

        // Reassemble soft-wrapped rows as logical lines, then wrap at the new
        // column count. Offscreen rows above the visible buffer are unavailable.
        let (cx, cy) = screen.cursor;
        let last = screen
            .cells
            .chunks_exact(screen.cols)
            .rposition(|line| line.iter().any(|&byte| byte != b' '))
            .unwrap_or(cy)
            .max(cy);
        let mut mapped = Vec::new();
        let mut wraps = Vec::new();
        let (mut x, mut y) = (0usize, 0usize);
        let mut cursor = (0, 0, false);
        for row in 0..=last {
            let start = row * screen.cols;
            let line = &screen.cells[start..start + screen.cols];
            let joins = row + 1 < screen.rows && screen.wrapped[row];
            let mut used = line
                .iter()
                .rposition(|&byte| byte != b' ')
                .map_or(0, |column| column + 1);
            if row == cy {
                used = used.max(cx + 1);
            }
            if joins {
                used = screen.cols;
            }
            for column in 0..used {
                if x == self.cols {
                    wraps.push(y);
                    y += 1;
                    x = 0;
                }
                if (column, row) == (cx, cy) {
                    let after = x + usize::from(screen.wrap_pending);
                    cursor = (after.min(self.cols - 1), y, after == self.cols);
                }
                mapped.push((start + column, x, y));
                x += 1;
            }
            if !joins {
                y += 1;
                x = 0;
            }
        }
        let first = y.saturating_sub(self.rows).min(cursor.1);
        self.styles.fill((self.color,self.background));
        self.cells.fill(b' ');
        self.wrapped.fill(false);
        for row in 0..self.rows * CH {
            fb[row * W..row * W + self.cols * CW].fill(self.background);
        }
        for row in wraps {
            if (first..first + self.rows).contains(&row) {
                self.wrapped[row - first] = true;
            }
        }
        for (old, col, row) in mapped {
            if !(first..first + self.rows).contains(&row) {
                continue;
            }
            let row = row - first;
            self.cells[row * self.cols + col] = screen.cells[old];
            self.styles[row*self.cols+col] = screen.styles.get(old).copied().unwrap_or(scrollback::DEFAULT_STYLE);
            let from = old / screen.cols * CH * (screen.cols * CW) + old % screen.cols * CW;
            let to = row * CH * W + col * CW;
            for pixel_row in 0..CH {
                let src = from + pixel_row * screen.cols * CW;
                let dst = to + pixel_row * W;
                fb[dst..dst + CW].copy_from_slice(&screen.pixels[src..src + CW]);
            }
        }
        (self.x, self.y, self.wrap_pending) = (cursor.0, cursor.1 - first, cursor.2);
    }
}
