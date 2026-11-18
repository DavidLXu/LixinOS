//! A single-buffer text editor for the guest's ANSI console.
use crate::{print, read_byte};
use alloc::{format, string::String, vec::Vec};
use core::fmt::Write;

const ROWS: usize = 18;

struct Buffer {
    lines: Vec<Vec<char>>,
    row: usize,
    col: usize,
    preferred: usize,
    top: usize,
    left: usize,
    bytes: usize,
    dirty: bool,
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

    fn draw(&mut self, name: &str, message: &str) {
        let columns = ((crate::geometry() & 65535) as usize - 2).min(78);
        let visual_col = self.lines[self.row][..self.col]
            .iter()
            .fold(0, |x, c| x + cell_width(*c, x));
        self.top = self.top.clamp(self.row.saturating_sub(ROWS - 1), self.row);
        self.left = self
            .left
            .clamp(visual_col.saturating_sub(columns - 1), visual_col);
        let mut frame = format!(
            "\x1b[H\x1b[7m LixinOS Editor   {}{}\x1b[0m\x1b[K",
            name.chars()
                .filter(|c| c.is_ascii_graphic() || *c == ' ')
                .take((columns - 21).min(48))
                .collect::<String>(),
            if self.dirty { "  *" } else { "" }
        );
        for i in 0..ROWS {
            frame.push_str("\n\x1b[K");
            if let Some(line) = self.lines.get(self.top + i) {
                let mut x = 0;
                for &c in line {
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
            }
        }
        write!(frame,"\n\x1b[KLn {}  Col {}   {}\n\x1b[K^O Save   ^X Exit   ^K Cut line   ^U Paste line\x1b[{};{}H",
            self.row+1,visual_col+1,message,self.row-self.top+2,visual_col-self.left+1).unwrap();
        print(&frame);
    }
}

fn cell_width(c: char, x: usize) -> usize {
    if c == '\t' { 4 - x % 4 } else { 1 }
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
        buf.draw(path, message);
        let k = crate::keys::emacs_key(crate::terminal_key());
        if core::mem::take(&mut skip_lf) && k == 10 {
            continue;
        }
        message = "";
        match k {
            24 | 15 => {
                if k == 24 {
                    if !buf.dirty {
                        break;
                    }
                    buf.draw(path, "Save changes? Y=yes  N=discard  Esc=cancel");
                    match read_byte() {
                        b'n' | b'N' => break,
                        b'y' | b'Y' => (),
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
                    25 => buf.row.saturating_sub(ROWS),
                    _ => buf.row + ROWS,
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
