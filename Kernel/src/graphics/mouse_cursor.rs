pub(super) const WIDTH: usize = 16;
pub(super) const HEIGHT: usize = 21;

pub(super) struct Overlay {
    underlay: [[[u32; 4]; WIDTH]; HEIGHT],
}

impl Overlay {
    pub(super) const fn new() -> Self {
        Self { underlay: [[[0; 4]; WIDTH]; HEIGHT] }
    }

    pub(super) fn paint(&mut self, fb: &mut [u32], stride: usize, w: usize, h: usize, text: bool, show: bool) {
        let shape = if text { &SHAPES[1] } else { &SHAPES[0] };
        for (y, row) in shape.iter().take(h / 2).enumerate() {
            for (x, &ink) in row.iter().take(w / 2).enumerate() {
                if ink == b'.' { continue; }
                let start = 2 * y * stride + 2 * x;
                let saved = &mut self.underlay[y][x];
                for (i, offset) in [0, 1, stride, stride + 1].into_iter().enumerate() {
                    let pixel = &mut fb[start + offset];
                    if show {
                        saved[i] = *pixel;
                        *pixel = color(ink, *pixel);
                    } else {
                        *pixel = saved[i];
                    }
                }
            }
        }
    }
}

const SHAPES: [[[u8; WIDTH]; HEIGHT]; 2] = {
    let source = include_bytes!("../../../User/gui/mouse-cursor.h");
    let mut shapes = [[[0; WIDTH]; HEIGHT]; 2];
    let mut index = 0;
    let mut y = 0;
    while y < 2 * HEIGHT {
        while source[index] != b'"' { index += 1; }
        index += 1;
        let mut x = 0;
        while x < WIDTH {
            let ink = source[index + x];
            assert!(matches!(ink, b'.' | b'O' | b'G' | b'g' | b'W' | b'+' | b'~' | b':'));
            shapes[y / HEIGHT][y % HEIGHT][x] = ink;
            x += 1;
        }
        index += WIDTH;
        assert!(source[index] == b'"');
        index += 1;
        y += 1;
    }
    shapes
};

fn color(ink: u8, background: u32) -> u32 {
    let outline = 0x1b2532;
    let shadow = 0x0c1219;
    match ink {
        b'O' => outline,
        b'G' => 0x627180,
        b'g' => 0xaebdca,
        b'W' => 0xf7fbff,
        b'+' => ((background & 0xfefefe) >> 1) + ((outline & 0xfefefe) >> 1),
        b'~' => background - ((background & 0xfcfcfc) >> 2) + ((outline & 0xfcfcfc) >> 2),
        b':' => background - ((background & 0xfcfcfc) >> 2) + ((shadow & 0xfcfcfc) >> 2),
        _ => background,
    }
}
