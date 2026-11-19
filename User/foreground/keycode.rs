// Translate a completed CSI/SS3 sequence to the editor's logical key.
pub fn keycode(byte: i32, parameter: i32) -> i32 {
    match byte {
        68 => 1000,
        67 => 1001,
        65 => 1002,
        66 => 1003,
        72 => 1005,
        70 => 1006,
        126 => match parameter {
            3 => 1004,
            1 => 1005,
            7 => 1005,
            4 => 1006,
            8 => 1006,
            5 => 1007,
            6 => 1008,
            _ => 27,
        },
        _ => 27,
    }
}
