// CSI and SS3 decoding shared by the shell and user-mode console programs.
pub fn decode(first: u8, mut next: impl FnMut() -> Option<u8>) -> i32 {
    if first != 27 {
        return first.into();
    }
    if !matches!(next(), Some(b'[' | b'O')) {
        return 27;
    }
    let mut parameter = 0u16;
    while let Some(byte) = next() {
        match byte {
            b'0'..=b'9' => {
                parameter = parameter
                    .saturating_mul(10)
                    .saturating_add((byte - b'0') as u16)
            }
            b'D' => return 1000,
            b'C' => return 1001,
            b'A' => return 1002,
            b'B' => return 1003,
            b'H' => return 1005,
            b'F' => return 1006,
            b'~' => {
                return match parameter {
                    3 => 1004,
                    1 | 7 => 1005,
                    4 | 8 => 1006,
                    5 => 1007,
                    6 => 1008,
                    _ => 27,
                };
            }
            _ => return 27,
        }
    }
    27
}
pub fn emacs_key(key: i32) -> u8 {
    match key {
        1000 => 2,
        1001 => 6,
        1002 => 16,
        1003 => 14,
        1004 => 4,
        1005 => 1,
        1006 => 5,
        1007 => 25,
        1008 => 22,
        27 => 0,
        c => c as u8,
    }
}
