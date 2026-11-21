//! Terminal controls used by local editors and remote xterm applications.
use super::{BG, CH, CW, FG, Terminal};

#[derive(Clone, Copy)]
pub(super) enum Parser {
    Ground,
    Escape,
    Csi(u8),
    Charset(Option<usize>),
    String { osc: bool, escape: bool },
}

const COLORS: [u32; 16] = [
    0x565e6b, 0xe69a96, 0x9dc69e, 0xe7cc91, 0x90b7ed, 0xcdabd9, 0x89c9ca, FG, 0x88939b, 0xffb2ae,
    0xbde5ba, 0xffdf9e, 0xb3cfff, 0xe9cfed, 0xa5edeb, 0xffffff,
];

impl Terminal {
    fn erase(&mut self, fb: &mut [u32], row: usize, start: usize, end: usize) {
        let background = if self.reverse {
            self.color
        } else {
            self.background
        };
        for py in row * CH..(row + 1) * CH {
            fb[py * self.stride + start * CW..py * self.stride + end * CW].fill(background);
        }
        self.cells[row * self.cols + start..row * self.cols + end].fill(b' ');
        self.styles[row*self.cols+start..row*self.cols+end].fill((self.color,background));
        if start == 0 && end == self.cols {
            self.wrapped[row] = false;
        }
    }

    fn erase_display(&mut self, fb: &mut [u32], mode: usize) {
        for row in 0..self.rows {
            match mode {
                0 if row >= self.y => {
                    self.erase(fb, row, if row == self.y { self.x } else { 0 }, self.cols)
                }
                1 if row <= self.y => self.erase(
                    fb,
                    row,
                    0,
                    if row == self.y { self.x + 1 } else { self.cols },
                ),
                2 => self.erase(fb, row, 0, self.cols),
                _ => (),
            }
        }
    }

    fn scroll(&mut self, fb: &mut [u32], first: usize, last: usize, count: usize, down: bool) {
        let height = last - first + 1;
        let count = count.min(height);
        if !down && first == 0 && last+1 == self.rows && self.alternate.is_none() {
            self.remember(count);
        }
        let remaining = height - count;
        for index in 0..remaining {
            let row = if down { remaining - 1 - index } else { index };
            let src = first + row + if down { 0 } else { count };
            let dst = first + row + if down { count } else { 0 };
            for y in 0..CH {
                let start = (src * CH + y) * self.stride;
                fb.copy_within(start..start + self.cols * CW, (dst * CH + y) * self.stride);
            }
            self.cells
                .copy_within(src * self.cols..(src + 1) * self.cols, dst * self.cols);
            self.styles.copy_within(src*self.cols..(src+1)*self.cols,dst*self.cols);
            self.wrapped[dst] = self.wrapped[src];
        }
        let blank = if down { first } else { first + remaining };
        for row in blank..blank + count {
            self.erase(fb, row, 0, self.cols);
        }
    }

    fn line_feed(&mut self, fb: &mut [u32], reverse: bool) {
        let (first, last) = self.margins.unwrap_or((0, self.rows - 1));
        if (!reverse && self.y == last) || (reverse && self.y == first) {
            self.scroll(fb, first, last, 1, reverse);
        } else if reverse {
            self.y = self.y.saturating_sub(1);
        } else {
            self.y = (self.y + 1).min(self.rows - 1);
        }
    }

    fn shift_columns(&mut self, fb: &mut [u32], count: usize, right: bool) {
        let count = count.min(self.cols - self.x);
        let src = self.x + if right { 0 } else { count };
        let dst = self.x + if right { count } else { 0 };
        let remaining = self.cols - self.x - count;
        for y in self.y * CH..(self.y + 1) * CH {
            let start = y * self.stride + src * CW;
            fb.copy_within(start..start + remaining * CW, y * self.stride + dst * CW);
        }
        let line = self.y * self.cols;
        self.cells
            .copy_within(line + src..line + src + remaining, line + dst);
        self.styles.copy_within(line+src..line+src+remaining,line+dst);
        let blank = if right { self.x } else { self.cols - count };
        self.erase(fb, self.y, blank, blank + count);
    }

    fn alternate(&mut self, fb: &mut [u32], enable: bool) {
        if enable {
            if self.alternate.is_some() {
                return;
            }
            self.alternate = Some(self.capture(fb));
            self.erase_display(fb, 2);
            (self.x, self.y) = (0, 0);
        } else if let Some(screen) = self.alternate.take() {
            self.restore(fb, screen);
        }
        self.margins = None;
    }

    fn csi(&mut self, fb: &mut [u32], prefix: u8, byte: u8) {
        if prefix == b'?' && matches!(byte, b'h' | b'l') {
            let enabled = byte == b'h';
            for i in 0..=self.param {
                match self.params[i] {
                    1 => self.application_cursor = enabled,
                    7 => {
                        self.auto_wrap = enabled;
                        self.wrap_pending = false;
                    }
                    25 => self.cursor_visible = enabled,
                    47 | 1047 | 1049 => {
                        self.alternate(fb, enabled);
                        self.wrap_pending = false;
                    }
                    1048 if enabled => self.saved_cursor = (self.x, self.y),
                    1048 => {
                        (self.x, self.y) = self.saved_cursor;
                        self.wrap_pending = false;
                    }
                    _ => (),
                }
            }
            return;
        }
        if prefix != 0 {
            return;
        }
        if byte != b'm' {
            self.wrap_pending = false;
        }
        let n = self.params[0].max(1);
        let (first, last) = self.margins.unwrap_or((0, self.rows - 1));
        match byte {
            b'H' | b'f' => {
                self.y = (n - 1).min(self.rows - 1);
                self.x = self.params[1].saturating_sub(1).min(self.cols - 1);
            }
            b'A' => self.y = self.y.saturating_sub(n),
            b'B' | b'e' => self.y = (self.y + n).min(self.rows - 1),
            b'C' | b'a' => self.x = (self.x + n).min(self.cols - 1),
            b'D' => self.x = self.x.saturating_sub(n),
            b'E' => {
                self.x = 0;
                self.y = (self.y + n).min(self.rows - 1);
            }
            b'F' => {
                self.x = 0;
                self.y = self.y.saturating_sub(n);
            }
            b'G' | b'`' => self.x = (n - 1).min(self.cols - 1),
            b'd' => self.y = (n - 1).min(self.rows - 1),
            b'J' if self.params[0] == 3 && self.alternate.is_none() => self.clear_history(),
            b'J' => self.erase_display(fb, self.params[0]),
            b'K' => match self.params[0] {
                0 => self.erase(fb, self.y, self.x, self.cols),
                1 => self.erase(fb, self.y, 0, self.x + 1),
                2 => self.erase(fb, self.y, 0, self.cols),
                _ => (),
            },
            b'X' => self.erase(fb, self.y, self.x, (self.x + n).min(self.cols)),
            b'@' | b'P' => self.shift_columns(fb, n, byte == b'@'),
            b'L' | b'M' if (first..=last).contains(&self.y) => {
                self.scroll(fb, self.y, last, n, byte == b'L')
            }
            b'S' | b'T' => self.scroll(fb, first, last, n, byte == b'T'),
            b's' => self.saved_cursor = (self.x, self.y),
            b'u' => (self.x, self.y) = self.saved_cursor,
            b'r' => {
                let end = if self.params[1] == 0 {
                    self.rows
                } else {
                    self.params[1]
                };
                if n < end && end <= self.rows {
                    self.margins = Some((n - 1, end - 1));
                    (self.x, self.y) = (0, 0);
                }
            }
            b'm' => {
                for i in 0..=self.param {
                    match self.params[i] {
                        0 => {
                            self.color = FG;
                            self.background = BG;
                            self.reverse = false;
                        }
                        7 => self.reverse = true,
                        27 => self.reverse = false,
                        p @ 30..=37 => self.color = COLORS[p - 30],
                        p @ 40..=47 => self.background = COLORS[p - 40],
                        p @ 90..=97 => self.color = COLORS[p - 90 + 8],
                        p @ 100..=107 => self.background = COLORS[p - 100 + 8],
                        39 => self.color = FG,
                        49 => self.background = BG,
                        _ => (),
                    }
                }
            }
            _ => (),
        }
    }

    fn print_cell(&mut self, fb: &mut [u32], byte: u8) {
        if self.wrap_pending {
            self.wrapped[self.y] = true;
            self.x = 0;
            self.line_feed(fb, false);
        }
        self.draw(fb, byte);
        self.wrap_pending = self.auto_wrap && self.x + 1 == self.cols;
        self.x = (self.x + 1).min(self.cols - 1);
    }

    pub(super) fn parse(&mut self, fb: &mut [u32], byte: u8) {
        match self.parser {
            Parser::Escape => {
                self.parser = match byte {
                    b'[' => {
                        self.params.fill(0);
                        self.param = 0;
                        Parser::Csi(0)
                    }
                    b'(' | b')' => Parser::Charset(Some(usize::from(byte == b')'))),
                    b'*' | b'+' | b'%' | b'#' => Parser::Charset(None),
                    b']' | b'P' | b'_' | b'^' => Parser::String {
                        osc: byte == b']',
                        escape: false,
                    },
                    27 => Parser::Escape,
                    _ => {
                        self.wrap_pending = false;
                        match byte {
                            b'7' => self.saved_cursor = (self.x, self.y),
                            b'8' => (self.x, self.y) = self.saved_cursor,
                            b'D' | b'M' => self.line_feed(fb, byte == b'M'),
                            b'E' => {
                                self.x = 0;
                                self.line_feed(fb, false);
                            }
                            _ => (),
                        }
                        Parser::Ground
                    }
                };
                return;
            }
            Parser::Csi(prefix) => {
                match byte {
                    b'0'..=b'9' => {
                        self.params[self.param] =
                            (self.params[self.param] * 10 + (byte - b'0') as usize).min(9999)
                    }
                    b';' => self.param = (self.param + 1).min(self.params.len() - 1),
                    b'?' | b'>' | b'=' | b'!' | b':' | b' ' => self.parser = Parser::Csi(byte),
                    0x40..=0x7e => {
                        self.parser = Parser::Ground;
                        self.csi(fb, prefix, byte);
                    }
                    27 => self.parser = Parser::Escape,
                    _ => (),
                }
                return;
            }
            Parser::Charset(bank) => {
                if let Some(bank) = bank {
                    self.charset[bank] = byte == b'0';
                }
                self.parser = Parser::Ground;
                return;
            }
            Parser::String { osc, escape } => {
                self.parser = if (escape && byte == b'\\') || (osc && byte == 7) {
                    Parser::Ground
                } else {
                    Parser::String {
                        osc,
                        escape: byte == 27,
                    }
                };
                return;
            }
            Parser::Ground => (),
        }
        if self.utf8_expected != 0 {
            if byte & 0xc0 == 0x80 {
                self.utf8[self.utf8_used] = byte;
                self.utf8_used += 1;
                if self.utf8_used == self.utf8_expected {
                    let scalar = core::str::from_utf8(&self.utf8[..self.utf8_used])
                        .ok()
                        .and_then(|text| text.chars().next());
                    self.utf8_expected = 0;
                    self.print_cell(
                        fb,
                        match scalar {
                            Some('─' | '━' | '═') => b'-',
                            Some('│' | '┃' | '║') => b'|',
                            Some('\u{2500}'..='\u{257f}') => b'+',
                            Some('\u{2580}'..='\u{259f}') => b'#',
                            _ => b'?',
                        },
                    );
                }
                return;
            }
            self.utf8_expected = 0;
        }
        if matches!(byte, 8..=13) {
            self.wrap_pending = false;
        }
        match byte {
            8 => self.x = self.x.saturating_sub(1),
            9 => self.x = ((self.x / 8 + 1) * 8).min(self.cols - 1),
            10..=12 => {
                self.wrapped[self.y] = false;
                self.line_feed(fb, false);
            }
            13 => self.x = 0,
            14 | 15 => self.active_charset = usize::from(byte == 14),
            27 => self.parser = Parser::Escape,
            32..=126 => {
                let glyph = if self.charset[self.active_charset] {
                    match byte {
                        b'q' => b'-',
                        b'x' => b'|',
                        b'j'..=b'n' | b't'..=b'w' => b'+',
                        b'a' => b'#',
                        _ => byte,
                    }
                } else {
                    byte
                };
                self.print_cell(fb, glyph);
            }
            0xc2..=0xf4 => {
                self.utf8[0] = byte;
                self.utf8_used = 1;
                self.utf8_expected = if byte < 0xe0 {
                    2
                } else if byte < 0xf0 {
                    3
                } else {
                    4
                };
            }
            _ => (),
        }
    }
}
