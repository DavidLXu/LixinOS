//! Bounded scrollback for the primary text screen of each terminal.
use super::*;
use alloc::{collections::VecDeque, vec::Vec};

pub(super) type Style = (u32, u32);
pub(super) const DEFAULT_STYLE: Style = (FG, BG);
const HISTORY_ROWS: usize = 1024;
const WHEEL_ROWS: usize = 3;

pub(super) struct History {
    rows: VecDeque<(Vec<u8>, Vec<Style>)>,
    pub(super) offset: usize,
}

impl History {
    pub(super) const fn new() -> Self {
        Self {
            rows: VecDeque::new(),
            offset: 0,
        }
    }
}

impl Terminal {
    pub(super) fn remember(&mut self, count: usize) {
        for row in 0..count {
            let range = row * self.cols..(row + 1) * self.cols;
            if self.history.rows.len() == HISTORY_ROWS {
                self.history.rows.pop_front();
            }
            let cells = &self.cells[range.clone()];
            let styles = &self.styles[range];
            let used = cells
                .iter()
                .zip(styles)
                .rposition(|(&ch, &style)| ch != b' ' || style.1 != BG)
                .map_or(0, |column| column + 1);
            self.history
                .rows
                .push_back((cells[..used].to_vec(), styles[..used].to_vec()));
        }
    }

    pub(super) fn clear_history(&mut self) {
        self.history.rows.clear();
    }

    pub(super) fn visible_row(&self, row: usize) -> (&[u8], &[Style]) {
        if row < self.history.offset {
            let (cells, styles) =
                &self.history.rows[self.history.rows.len() - self.history.offset + row];
            (cells, styles)
        } else {
            let start = (row - self.history.offset) * self.cols;
            (
                &self.cells[start..start + self.cols],
                self.styles.get(start..start + self.cols).unwrap_or(&[]),
            )
        }
    }

    // Caller has removed transient overlays. Shift the overlap and rasterize
    // only newly exposed rows, including the background of blank cells.
    pub(super) fn viewport(&mut self, fb: &mut [u32], offset: usize) {
        let old = self.history.offset;
        self.history.offset = offset;
        let shift = old.abs_diff(offset).min(self.rows);
        let width = self.cols * CW;
        let up = offset > old;
        for i in 0..(self.rows - shift) * CH {
            let y = if up {
                (self.rows - shift) * CH - 1 - i
            } else {
                i
            };
            let (src, dst) = if up {
                (y, y + shift * CH)
            } else {
                (y + shift * CH, y)
            };
            fb.copy_within(src * W..src * W + width, dst * W);
        }
        let first = if up { 0 } else { self.rows - shift };
        for row in first..first + shift {
            let (cells, styles) = self.visible_row(row);
            for col in 0..self.cols {
                let byte = cells.get(col).copied().unwrap_or(b' ');
                let (fg, bg) = styles.get(col).copied().unwrap_or(DEFAULT_STYLE);
                let start = row * CH * W + col * CW;
                if byte == b' ' {
                    for dy in 0..CH {
                        fb[start + dy * W..start + dy * W + CW].fill(bg);
                    }
                } else {
                    glyph(fb, start, CW, byte, fg, bg);
                }
            }
        }
    }

    pub(super) fn wheel(&mut self, fb: &mut [u32], event: u64) {
        if self.alternate.is_some() {
            return;
        }
        let offset = match (event >> 26) & 3 {
            1 => (self.history.offset + WHEEL_ROWS).min(self.history.rows.len()),
            2 => self.history.offset.saturating_sub(WHEEL_ROWS),
            _ => return,
        };
        if offset != self.history.offset {
            self.clear_selection(fb);
            self.cursor(fb);
            self.viewport(fb, offset);
            self.cursor(fb);
        }
    }
}
