#[cfg(feature = "split")]
pub const SWITCH_PANE: u8 = 0xfe;
use crate::read_byte;

// Cursor keys can arrive as CSI or SS3 sequences. A lone Escape must time out.
fn escape_byte() -> Option<u8> {
    let start = crate::monitor::ticks();
    let frequency: u64;
    unsafe {
        core::arch::asm!("mrs {}, cntfrq_el0",out(reg)frequency);
    }
    while crate::monitor::ticks().wrapping_sub(start) < frequency / 10 {
        if let Some(byte) = crate::poll_byte() {
            return Some(byte);
        }
    }
    None
}

pub fn read_key() -> u8 {
    let byte = read_byte();
    if byte != 27 {
        return byte;
    }
    if !matches!(escape_byte(), Some(b'[' | b'O')) {
        return 0;
    }
    let mut parameter = 0u16;
    while let Some(c) = escape_byte() {
        match c {
            b'0'..=b'9' => {
                parameter = parameter
                    .saturating_mul(10)
                    .saturating_add(u16::from(c - b'0'))
            }
            b'A' => return 16,
            b'B' => return 14,
            b'C' => return 6,
            b'D' => return 2,
            b'H' => return 1,
            b'F' => return 5,
            b'~' => {
                return match parameter {
                    1 | 7 => 1,
                    4 | 8 => 5,
                    3 => 4,
                    5 => 25,
                    6 => 22,
                    _ => 0,
                };
            }
            _ => return 0,
        }
    }
    0
}
