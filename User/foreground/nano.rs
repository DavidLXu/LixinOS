//! A single-buffer text editor for the guest's ANSI console.
use crate::{print, read_byte};
use alloc::{format, string::String, vec::Vec};
use core::fmt::Write;

unsafe extern "C" {
    fn editor_event() -> i32;
    fn editor_copy(bytes: *const u8, len: usize) -> i32;
    fn editor_paste() -> i32;
    fn editor_mailbox() -> *const u8;
}
type Position = (usize, usize);

const TITLE_STYLE: &str = "\x1b[36;7m";
const STATUS_STYLE: &str = "\x1b[37;40m";
const HELP_STYLE: &str = "\x1b[96;40m";
const SAVE_QUESTION: &str = "Save changes? Y=yes  N=discard  Esc=cancel";
const CONSOLE_HELP: &str = "^O Save   ^X Exit   ^K Cut line   ^U Paste line";
const ACTIONS: [(&str, i32); 5] = [
    ("^S Save", 19),
    ("^C Copy", 3),
    ("^X Cut", 24),
    ("^V Paste", 22),
    ("^Q Exit", 17),
];
const COMPACT_ACTIONS: [(&str, i32); 5] = [
    ("^S", 19),
    ("^C", 3),
    ("^X", 24),
    ("^V", 22),
    ("^Q", 17),
];

#[derive(Clone, Copy, PartialEq, Eq)]
struct Layout {
    cols: usize,
    rows: usize,
}

impl Layout {
    fn current() -> Self {
        let geometry = crate::geometry();
        Self {
            cols: ((geometry & 65535) as usize).max(4),
            rows: (((geometry >> 16) & 65535) as usize).max(4),
        }
    }

    fn body_rows(self) -> usize { self.rows - 3 }

    fn actions(self) -> &'static [(&'static str, i32)] {
        if self.cols >= 45 { &ACTIONS } else { &COMPACT_ACTIONS }
    }
}

fn bar(frame: &mut String, row: usize, style: &str, text: &str, cols: usize) {
    let text: String = text.chars().take(cols.saturating_sub(1)).collect();
    write!(frame, "\x1b[{row};1H{style}\x1b[2K {text}\x1b[0m").unwrap();
}

struct Buffer {
    lines: Vec<Vec<char>>,
    row: usize,
    col: usize,
    preferred: usize,
    top: usize,
    left: usize,
    bytes: usize,
    dirty: bool,
    anchor: Option<Position>,
    desktop: bool,
    dragging: bool,
    cut: Option<(Vec<char>, bool)>,
}

impl Buffer {
    fn new(text: &str) -> Self {
        Self {
            lines: text.split('\n').map(|s| s.chars().collect()).collect(),
            row: 0,
            col: 0,
            preferred: 0,
            top: 0,
            left: 0,
            bytes: text.len(),
            dirty: false,
            cut: None,
            desktop: crate::geometry() >> 33 != 0,
            anchor: None,
            dragging: false,
        }
    }

    fn text(&self) -> String {
        let mut out = String::with_capacity(self.bytes);
        for (i, line) in self.lines.iter().enumerate() {
            if i > 0 {
                out.push('\n');
            }
            out.extend(line);
        }
        out
    }

    fn vertical(&mut self, row: usize) {
        self.row = row.min(self.lines.len() - 1);
        self.col = self.preferred.min(self.lines[self.row].len());
    }

    fn delete(&mut self) {
        if self.col < self.lines[self.row].len() {
            self.bytes -= self.lines[self.row].remove(self.col).len_utf8();
        } else if self.row + 1 < self.lines.len() {
            let next = self.lines.remove(self.row + 1);
            self.lines[self.row].extend(next);
            self.bytes -= 1;
        } else {
            return;
        }
        self.dirty = true;
    }

    fn left(&mut self) {
        if self.col > 0 {
            self.col -= 1;
        } else if self.row > 0 {
            self.row -= 1;
            self.col = self.lines[self.row].len();
        }
    }

    fn selection(&self) -> Option<(Position, Position)> {
        let anchor = self.anchor?;
        let caret = (self.row, self.col);
        (anchor != caret).then_some((anchor.min(caret), anchor.max(caret)))
    }

    fn selected_text(&self) -> String {
        let mut out = String::new();
        if let Some(((r, c), (end, last))) = self.selection() {
            for row in r..=end {
                if row > r {
                    out.push('\n');
                }
                let line = &self.lines[row];
                out.extend(
                    &line[if row == r { c } else { 0 }..if row == end { last } else { line.len() }],
                );
            }
        }
        out
    }

    fn replace_selection(&mut self, text: &str, limit: usize) -> bool {
        let caret = (self.row, self.col);
        let ((r, c), (end, last)) = self.selection().unwrap_or((caret, caret));
        let bytes = self.bytes - self.selected_text().len() + text.len();
        if bytes > limit {
            return false;
        }
        let tail = self.lines[end].split_off(last);
        self.lines[r].truncate(c);
        self.lines.drain(r + 1..end + 1);
        let mut parts = text.split('\n');
        self.lines[r].extend(parts.next().unwrap().chars());
        let added: Vec<Vec<char>> = parts.map(|s| s.chars().collect()).collect();
        self.row = r + added.len();
        self.lines.splice(r + 1..r + 1, added);
        self.col = self.lines[self.row].len();
        self.lines[self.row].extend(tail);
        self.preferred = self.col;
        self.bytes = bytes;
        self.dirty = true;
        self.anchor = None;
        true
    }

    fn mouse(&mut self, event: i32, layout: Layout) -> Option<i32> {
        let phase = event >> 20;
        let y = ((event >> 8) & 255) as usize;
        let x = (event & 255) as usize;
        let rows = layout.body_rows();
        if phase == 1 {
            self.dragging = (1..=rows).contains(&y);
            if y == layout.rows - 1 {
                let mut left = 1;
                for &(label, key) in layout.actions() {
                    if (left..left + label.len()).contains(&x) {
                        return Some(key);
                    }
                    left += label.len() + 2;
                }
            }
        }
        if !self.dragging {
            return None;
        }
        self.dragging = phase != 3;
        self.row = match y {
            0 => self.top.saturating_sub(1),
            y if y > rows => self.top + rows,
            y => self.top + y - 1,
        }
        .min(self.lines.len() - 1);
        let target = self.left + x.saturating_sub(1);
        let mut visual = 0;
        self.col = 0;
        for &ch in &self.lines[self.row] {
            let w = cell_width(ch, visual);
            if target < visual + w {
                break;
            }
            visual += w;
            self.col += 1;
        }
        self.preferred = self.col;
        if phase == 1 {
            self.anchor = Some((self.row, self.col));
        }
        None
    }

    fn draw(&mut self, name: &str, message: &str, layout: Layout) {
        let columns = layout.cols - 2;
        let rows = layout.body_rows();
        let visual_col = self.lines[self.row][..self.col]
            .iter()
            .fold(0, |x, c| x + cell_width(*c, x));
        self.top = self.top.clamp(self.row.saturating_sub(rows - 1), self.row)
            .min(self.lines.len().saturating_sub(rows));
        self.left = self
            .left
            .clamp(visual_col.saturating_sub(columns - 1), visual_col);
        let name: String = name.chars()
            .map(|c| if c.is_ascii_graphic() || c == ' ' { c } else { '?' })
            .collect();
        let title = "Nano  ";
        let available = layout.cols.saturating_sub(title.len() + 3);
        let filename = if name.len() > available && available >= 4 {
            format!("...{}", &name[name.len() - available + 3..])
        } else {
            String::from(&name[..name.len().min(available)])
        };
        let mut frame = String::from("\x1b[?25l");
        bar(&mut frame, 1, TITLE_STYLE, &format!("{title}{filename}"), layout.cols);
        if self.dirty {
            write!(frame, "\x1b[1;{}H{TITLE_STYLE}*\x1b[0m", layout.cols - 1).unwrap();
        }
        let selection = self.selection();
        for i in 0..rows {
            write!(frame, "\x1b[{};1H\x1b[2K ", i + 2).unwrap();
            if let Some(line) = self.lines.get(self.top + i) {
                let mut x = 0;
                let mut highlight = false;
                for (col, &c) in line.iter().enumerate() {
                    let at = (self.top + i, col);
                    let selected = selection.is_some_and(|(a, b)| a <= at && at < b);
                    if selected != highlight {
                        frame.push_str(if selected { "\x1b[7m" } else { "\x1b[0m" });
                        highlight = selected;
                    }
                    let width = cell_width(c, x);
                    for pos in x..x + width {
                        if (self.left..self.left + columns).contains(&pos) {
                            frame.push(if c == '\t' {
                                ' '
                            } else if c.is_ascii_graphic() || c == ' ' {
                                c
                            } else {
                                '?'
                            });
                        }
                    }
                    x += width;
                    if x >= self.left + columns {
                        break;
                    }
                }
                if highlight {
                    frame.push_str("\x1b[0m");
                }
                let newline = (self.top + i, line.len());
                if (self.left..self.left + columns).contains(&x)
                    && selection.is_some_and(|(a, b)| a <= newline && newline < b)
                {
                    frame.push_str("\x1b[7m \x1b[0m");
                }
            }
        }
        let help = if self.desktop {
            layout.actions().iter().map(|&(label, _)| label).collect::<Vec<_>>().join("  ")
        } else if layout.cols > CONSOLE_HELP.len() {
            String::from(CONSOLE_HELP)
        } else {
            String::from("^O ^X ^K ^U")
        };
        let position = format!("Ln {}  Col {}", self.row + 1, visual_col + 1);
        let status = if message.is_empty() {
            if position.len() < layout.cols { position }
            else { format!("{}:{}", self.row + 1, visual_col + 1) }
        } else if position.len() + message.len() + 4 <= layout.cols {
            format!("{position}   {message}")
        } else if message == SAVE_QUESTION && message.len() >= layout.cols {
            String::from("Save? Y/N")
        } else {
            String::from(message)
        };
        bar(&mut frame, layout.rows - 1, STATUS_STYLE, &status, layout.cols);
        bar(&mut frame, layout.rows, HELP_STYLE, &help, layout.cols);
        write!(
            frame,
            "\x1b[{};{}H\x1b[?25h",
            self.row - self.top + 2,
            visual_col - self.left + 2
        )
        .unwrap();
        print(&frame);
    }
}

fn cell_width(c: char, x: usize) -> usize {
    if c == '\t' { 4 - x % 4 } else { 1 }
}

fn console_key(layout: Layout) -> Option<i32> {
    loop {
        let raw = unsafe { crate::read_timed(100) };
        if let Ok(byte) = u8::try_from(raw) {
            return Some(crate::keys::decode(byte, || {
                u8::try_from(unsafe { crate::read_timed(100) }).ok()
            }));
        }
        if Layout::current() != layout {
            return None;
        }
    }
}

pub fn edit(
    path: &str,
    text: &str,
    limit: usize,
    mut save: impl FnMut(String) -> Result<(), &'static str>,
) {
    let mut buf = Buffer::new(text);
    let mut message = "";
    if !(crate::geometry() >> 32 != 0) {
        print("\x1b[?1049h");
    }
    print("\x1b[2J");
    let mut skip_lf = false;
    loop {
        let layout = Layout::current();
        buf.draw(path, message, layout);
        let raw = if buf.desktop {
            let event = unsafe { editor_event() };
            if event >= 1 << 20 {
                let Some(action) = buf.mouse(event, layout) else {
                    continue;
                };
                action
            } else {
                crate::keys::decode(event as u8, || {
                    u8::try_from(unsafe { crate::read_timed(100) }).ok()
                })
            }
        } else {
            let Some(key) = console_key(layout) else { continue; };
            key
        };
        message = "";
        if buf.desktop {
            match raw {
                3 | 24 => {
                    if buf.selection().is_some() {
                        let text = buf.selected_text();
                        if unsafe { editor_copy(text.as_ptr(), text.len()) } != 0 {
                            message = "Clipboard unavailable";
                        } else if raw == 24 {
                            buf.replace_selection("", limit);
                            message = "Cut";
                        } else {
                            message = "Copied";
                        }
                    }
                    continue;
                }
                22 => {
                    let len = unsafe { editor_paste() };
                    if len > 0 {
                        let bytes =
                            unsafe { core::slice::from_raw_parts(editor_mailbox(), len as usize) };
                        if let Ok(text) = core::str::from_utf8(bytes) {
                            if !buf.replace_selection(text, limit) {
                                message = "File size limit";
                            }
                        }
                    }
                    continue;
                }
                1 => {
                    buf.anchor = Some((0, 0));
                    buf.row = buf.lines.len() - 1;
                    buf.col = buf.lines[buf.row].len();
                    buf.preferred = buf.col;
                    continue;
                }
                8 | 127 | 1004 if buf.selection().is_some() => {
                    buf.replace_selection("", limit);
                    continue;
                }
                9 | 10 | 13 | 32..=126 if buf.selection().is_some() => {
                    let byte = [if raw == 13 { 10 } else { raw as u8 }];
                    if !buf.replace_selection(core::str::from_utf8(&byte).unwrap(), limit) {
                        message = "File size limit";
                    }
                    skip_lf = raw == 13;
                    continue;
                }
                _ => buf.anchor = None,
            }
        }
        let k = match raw {
            17 if buf.desktop => 24,
            19 if buf.desktop => 15,
            key => crate::keys::emacs_key(key),
        };
        if core::mem::take(&mut skip_lf) && k == 10 {
            continue;
        }
        match k {
            24 | 15 => {
                if k == 24 {
                    if !buf.dirty {
                        break;
                    }
                    let answer = loop {
                        let layout = Layout::current();
                        buf.draw(path, SAVE_QUESTION, layout);
                        if buf.desktop {
                            break i32::from(read_byte());
                        }
                        if let Some(key) = console_key(layout) {
                            break key;
                        }
                    };
                    match u8::try_from(answer) {
                        Ok(b'n' | b'N') => break,
                        Ok(b'y' | b'Y') => (),
                        _ => continue,
                    }
                }
                match save(buf.text()) {
                    Ok(()) => {
                        buf.dirty = false;
                        if k == 24 {
                            break;
                        }
                        message = "Saved";
                    }
                    Err(err) => message = err,
                }
            }
            16 | 14 | 25 | 22 => {
                let row = match k {
                    16 => buf.row.saturating_sub(1),
                    14 => buf.row + 1,
                    25 => buf.row.saturating_sub(layout.body_rows()),
                    _ => buf.row + layout.body_rows(),
                };
                buf.vertical(row);
                continue;
            }
            1 => buf.col = 0,
            5 => buf.col = buf.lines[buf.row].len(),
            2 => buf.left(),
            6 => {
                if buf.col < buf.lines[buf.row].len() {
                    buf.col += 1
                } else if buf.row + 1 < buf.lines.len() {
                    buf.row += 1;
                    buf.col = 0
                }
            }
            4 => buf.delete(),
            8 | 127 => {
                if buf.col > 0 || buf.row > 0 {
                    buf.left();
                    buf.delete()
                }
            }
            11 => {
                let has_newline = buf.row + 1 < buf.lines.len();
                let line = if has_newline {
                    buf.lines.remove(buf.row)
                } else {
                    core::mem::take(&mut buf.lines[buf.row])
                };
                if line.is_empty() && !has_newline {
                    continue;
                }
                buf.bytes -=
                    line.iter().map(|c| c.len_utf8()).sum::<usize>() + usize::from(has_newline);
                buf.col = 0;
                buf.cut = Some((line, has_newline));
                buf.dirty = true;
            }
            21 => {
                if let Some((line, has_newline)) = &buf.cut {
                    let add = line.iter().map(|c| c.len_utf8()).sum::<usize>()
                        + usize::from(*has_newline);
                    if buf.bytes + add > limit {
                        message = "File size limit";
                        continue;
                    }
                    let tail = buf.lines[buf.row].split_off(buf.col);
                    buf.lines[buf.row].extend_from_slice(line);
                    if *has_newline {
                        buf.row += 1;
                        buf.col = 0;
                        buf.lines.insert(buf.row, tail);
                    } else {
                        buf.col += line.len();
                        buf.lines[buf.row].extend(tail);
                    }
                    buf.bytes += add;
                    buf.dirty = true;
                }
            }
            9 | 10 | 13 | 32..=126 => {
                if buf.bytes >= limit {
                    message = "File size limit";
                    continue;
                }
                if k == 10 || k == 13 {
                    skip_lf = k == 13;
                    let tail = buf.lines[buf.row].split_off(buf.col);
                    buf.row += 1;
                    buf.lines.insert(buf.row, tail);
                    buf.col = 0;
                } else {
                    buf.lines[buf.row].insert(buf.col, char::from(k));
                    buf.col += 1
                }
                buf.bytes += 1;
                buf.dirty = true;
            }
            _ => (),
        }
        buf.preferred = buf.col;
    }
    if crate::geometry() >> 32 != 0 {
        print("\x1b[2J\x1b[H")
    } else {
        print("\x1b[?1049l")
    }
}
