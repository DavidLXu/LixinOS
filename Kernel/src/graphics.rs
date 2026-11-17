//! QEMU virt ramfb and keyboard. One CPU, interrupts masked throughout.
use crate::disk::Dma;
use core::ptr::{NonNull, addr_of, addr_of_mut, read_volatile, write_volatile};
use virtio_drivers::{device::input::VirtIOInput, transport::mmio::MmioTransport};

const CW: usize = 22;
const CH: usize = 44;
const PANES: usize = if cfg!(feature = "split") { 2 } else { 1 };
pub const COLS: usize = if cfg!(feature = "split") { 52 } else { 82 };
const SCREEN_COLS: usize = COLS * PANES + PANES - 1;
const PANE_WIDTH: usize = COLS * CW;
const ROWS: usize = 24;
const BAR_PIXELS: usize = W * CH;
const W: usize = CW * SCREEN_COLS;
const H: usize = CH * (ROWS + 1);
const BG: u32 = 0x171b20;
const FG: u32 = 0xdadbd5;
const FONT: &[u8; 95 * CW * CH] = include_bytes!(concat!(env!("OUT_DIR"), "/font.gray"));
#[repr(align(4096))]
struct Frame([u32; W * H]);
static mut FB: Frame = Frame([0; W * H]);

fn configure_ramfb() {
    // fw_cfg MMIO: select file directory, find etc/ramfb, DMA-write its record.
    let data = 0x0902_0000 as *const u8;
    unsafe { write_volatile(0x0902_0008 as *mut u16, 0x19u16.to_be()) };
    let read = || unsafe { read_volatile(data) };
    let count = u32::from_be_bytes(core::array::from_fn(|_| read()));
    for _ in 0..count {
        let entry: [u8; 64] = core::array::from_fn(|_| read());
        if !entry[8..].starts_with(b"etc/ramfb\0") {
            continue;
        }
        let mut record = [0u8; 28];
        record[..8].copy_from_slice(&(addr_of!(FB) as u64).to_be_bytes());
        for (out, value) in record[8..].chunks_exact_mut(4).zip([
            u32::from_le_bytes(*b"XR24"),
            0,
            W as u32,
            H as u32,
            (W * 4) as u32,
        ]) {
            out.copy_from_slice(&value.to_be_bytes());
        }
        #[repr(C, align(8))]
        struct Transfer {
            control: u32,
            length: u32,
            address: u64,
        }
        let selector = u16::from_be_bytes([entry[4], entry[5]]) as u32;
        let mut transfer = Transfer {
            control: ((selector << 16) | 8 | 16).to_be(),
            length: 28u32.to_be(),
            address: (record.as_ptr() as u64).to_be(),
        };
        unsafe {
            core::arch::asm!("dsb sy");
            write_volatile(
                0x0902_0010 as *mut u64,
                (addr_of_mut!(transfer) as u64).to_be(),
            );
            while u32::from_be(read_volatile(addr_of!(transfer.control))) & !1 != 0 {
                core::hint::spin_loop();
            }
            assert_eq!(read_volatile(addr_of!(transfer.control)), 0);
        }
        return;
    }
    panic!("missing QEMU ramfb")
}

#[derive(Clone, Copy)]
struct Terminal {
    x: usize,
    y: usize,
    color: u32,
    wrap_pending: bool,
    reverse: bool,
    escape: bool,
    csi: bool,
    params: [usize; 4],
    param: usize,
}
static mut ACTIVE: usize = 0;
static mut TERMS: [Terminal; PANES] = [Terminal {
    x: 0,
    y: 0,
    color: FG,
    wrap_pending: false,
    reverse: false,
    escape: false,
    csi: false,
    params: [0; 4],
    param: 0,
}; PANES];

fn glyph(fb: &mut [u32], offset: usize, byte: u8, fg: u32, bg: u32) {
    let glyph = &FONT[(byte - 32) as usize * CW * CH..];
    for row in 0..CH {
        let start = offset + row * W;
        for (x, p) in fb[start..start + CW].iter_mut().enumerate() {
            let a = glyph[row * CW + x] as u32;
            let mix = |shift: u32| {
                ((((fg >> shift) & 255) * a + ((bg >> shift) & 255) * (255 - a)) / 255) << shift
            };
            *p = mix(16) | mix(8) | mix(0);
        }
    }
}
impl Terminal {
    fn draw(&self, fb: &mut [u32], byte: u8) {
        let (fg, bg) = if self.reverse {
            (BG, self.color)
        } else {
            (self.color, BG)
        };
        glyph(fb, self.y * CH * W + self.x * CW, byte, fg, bg);
    }
    fn newline(&mut self, fb: &mut [u32]) {
        if self.y + 1 < ROWS {
            self.y += 1;
        } else {
            if PANES == 1 {
                fb.copy_within(W * CH.., 0);
                fb[(ROWS - 1) * W * CH..].fill(BG);
            } else {
                for y in 0..(ROWS - 1) * CH {
                    let src = (y + CH) * W;
                    fb.copy_within(src..src + PANE_WIDTH, y * W);
                }
                for row in fb[(ROWS - 1) * W * CH..].chunks_mut(W) {
                    row[..PANE_WIDTH].fill(BG);
                }
            }
        }
    }
    fn control(&mut self, fb: &mut [u32], byte: u8) {
        if byte != b'm' {
            self.wrap_pending = false;
        }
        let n = self.params[0].max(1);
        match byte {
            b'H' | b'f' => {
                self.y = (n - 1).min(ROWS - 1);
                self.x = self.params[1].saturating_sub(1).min(COLS - 1);
            }
            b'A' => self.y = self.y.saturating_sub(n),
            b'B' => self.y = (self.y + n).min(ROWS - 1),
            b'C' => self.x = (self.x + n).min(COLS - 1),
            b'D' => self.x = self.x.saturating_sub(n),
            b'J' if self.params[0] == 2 => {
                for row in fb.chunks_mut(W) {
                    row[..PANE_WIDTH].fill(BG);
                }
            }
            b'K' => {
                for row in self.y * CH..(self.y + 1) * CH {
                    fb[row * W + self.x * CW..row * W + PANE_WIDTH].fill(BG);
                }
            }
            b'm' => {
                for &p in &self.params[..=self.param] {
                    match p {
                        0 => {
                            self.color = FG;
                            self.reverse = false;
                        }
                        7 => self.reverse = true,
                        32 => self.color = 0x9dc69e,
                        36 => self.color = 0x89c9ca,
                        _ => (),
                    }
                }
            }
            _ => (),
        }
    }
    fn byte(&mut self, fb: &mut [u32], b: u8) {
        // Cursor is a reversible underline, including over existing text.
        self.cursor(fb);
        if self.escape {
            self.escape = false;
            self.csi = b == b'[';
            self.params = [0; 4];
            self.param = 0;
        } else if self.csi {
            match b {
                b'0'..=b'9' => {
                    self.params[self.param] =
                        (self.params[self.param] * 10 + (b - b'0') as usize).min(9999)
                }
                b';' => self.param = (self.param + 1).min(3),
                b'?' => (),
                _ => {
                    self.csi = false;
                    self.control(fb, b);
                }
            }
        } else {
            if matches!(b, 8..=13) {
                self.wrap_pending = false;
            }
            match b {
                9 => self.x = ((self.x / 8 + 1) * 8).min(COLS - 1),
                27 => self.escape = true,
                8 => self.x = self.x.saturating_sub(1),
                13 => self.x = 0,
                10 => self.newline(fb),
                32..=126 => {
                    if self.wrap_pending {
                        self.x = 0;
                        self.newline(fb);
                    }
                    self.draw(fb, b);
                    self.wrap_pending = self.x + 1 == COLS;
                    self.x = (self.x + 1).min(COLS - 1);
                }
                _ => (),
            }
        }
        self.cursor(fb);
    }
    fn cursor(&self, fb: &mut [u32]) {
        let start = ((self.y + 1) * CH - 3) * W + self.x * CW;
        for p in &mut fb[start..start + CW] {
            *p ^= 0xb0b0b0;
        }
    }
}
pub fn put(byte: u8) {
    sample();
    unsafe {
        let fb = &mut (*addr_of_mut!(FB)).0;
        TERMS[ACTIVE].byte(&mut fb[BAR_PIXELS + ACTIVE * (PANE_WIDTH + CW)..], byte);
    }
}

#[cfg(feature = "split")]
pub fn switch_pane() {
    sample();
    unsafe {
        let fb = &mut (*addr_of_mut!(FB)).0;
        TERMS[ACTIVE].cursor(&mut fb[BAR_PIXELS + ACTIVE * (PANE_WIDTH + CW)..]);
        // Each suspended continuation may be waiting for input or doing work.
        static mut IDLE: [bool; 2] = [false; 2];
        IDLE[ACTIVE] = USAGE.waiting;
        ACTIVE ^= 1;
        USAGE.waiting = IDLE[ACTIVE];
        TERMS[ACTIVE].cursor(&mut fb[BAR_PIXELS + ACTIVE * (PANE_WIDTH + CW)..]);
        focus_label(&mut fb[..BAR_PIXELS]);
    }
}
#[cfg(feature = "split")]
fn focus_label(bar: &mut [u32]) {
    let label = if unsafe { ACTIVE } == 0 {
        "Left  | Ctrl+Tab"
    } else {
        "Right | Ctrl+Tab"
    };
    for (i, b) in label.bytes().enumerate() {
        glyph(bar, (12 + i) * CW, b, 0xcbdedb, 0x273038);
    }
}

struct Keyboard {
    dev: VirtIOInput<Dma, MmioTransport<'static>>,
    modifiers: u8,
    caps: bool,
    pending: &'static [u8],
}
static mut KEYBOARD: Option<Keyboard> = None;
pub fn init() {
    configure_ramfb();
    unsafe {
        let fb = &mut (*addr_of_mut!(FB)).0;
        fb.fill(BG);
        TERMS[0].cursor(&mut fb[BAR_PIXELS..]);
        if PANES == 2 {
            for row in fb[BAR_PIXELS..].chunks_exact_mut(W) {
                row[PANE_WIDTH + CW / 2..PANE_WIDTH + CW / 2 + 2].fill(0x44525b);
            }
        }
        start_status();
        let transport =
            MmioTransport::new(NonNull::new(0x0a00_0200 as *mut _).unwrap(), 512).unwrap();
        *addr_of_mut!(KEYBOARD) = Some(Keyboard {
            dev: VirtIOInput::new(transport).unwrap(),
            modifiers: 0,
            caps: false,
            pending: &[],
        });
    }
}
pub fn key() -> Option<u8> {
    let k = unsafe { (&mut *addr_of_mut!(KEYBOARD)).as_mut()? };
    loop {
        if let Some((&first, rest)) = k.pending.split_first() {
            k.pending = rest;
            return Some(first);
        }
        let e = k.dev.pop_pending_event()?;
        if e.event_type != 1 {
            continue;
        }
        let mask = match e.code {
            42 => 1,
            54 => 2,
            29 => 4,
            97 => 8,
            _ => 0,
        };
        if mask != 0 {
            if e.value == 0 {
                k.modifiers &= !mask;
            } else {
                k.modifiers |= mask;
            }
            continue;
        }
        if e.value == 0 {
            continue;
        }
        let shift = k.modifiers & 3 != 0;
        let ctrl = k.modifiers & 12 != 0;
        #[cfg(feature = "split")]
        if ctrl && e.code == 15 {
            if e.value == 1 {
                return Some(crate::keyboard::SWITCH_PANE);
            }
            continue;
        }
        let pair = match e.code {
            1 => return Some(27),
            14 => return Some(127),
            15 => return Some(9),
            28 | 96 => return Some(13),
            57 => return Some(32),
            58 => {
                if e.value == 1 {
                    k.caps = !k.caps;
                }
                continue;
            }
            102..=111 => {
                k.pending = match e.code {
                    102 => b"\x1b[H",
                    103 => b"\x1b[A",
                    104 => b"\x1b[5~",
                    105 => b"\x1b[D",
                    106 => b"\x1b[C",
                    107 => b"\x1b[F",
                    108 => b"\x1b[B",
                    109 => b"\x1b[6~",
                    110 => b"\x1b[2~",
                    111 => b"\x1b[3~",
                    _ => unreachable!(),
                };
                continue;
            }
            2..=13 => (
                b"1234567890-="[usize::from(e.code - 2)],
                b"!@#$%^&*()_+"[usize::from(e.code - 2)],
            ),
            16..=27 => (
                b"qwertyuiop[]"[usize::from(e.code - 16)],
                b"QWERTYUIOP{}"[usize::from(e.code - 16)],
            ),
            30..=41 => (
                b"asdfghjkl;'`"[usize::from(e.code - 30)],
                b"ASDFGHJKL:\"~"[usize::from(e.code - 30)],
            ),
            43 => (92, 124),
            44..=53 => (
                b"zxcvbnm,./"[usize::from(e.code - 44)],
                b"ZXCVBNM<>?"[usize::from(e.code - 44)],
            ),
            _ => continue,
        };
        let b = if shift ^ (k.caps && pair.0.is_ascii_alphabetic()) {
            pair.1
        } else {
            pair.0
        };
        return Some(if ctrl { b.to_ascii_uppercase() & 31 } else { b });
    }
}

// All updates run synchronously on the only vCPU. Waiting covers the shell,
// editor and REPL input loops and the monitor's refresh delay.
struct Usage {
    frequency: u64,
    start: u64,
    last: u64,
    idle: u64,
    waiting: bool,
    busy_percent: Option<u64>,
}
static mut USAGE: Usage = Usage {
    frequency: 0,
    start: 0,
    last: 0,
    idle: 0,
    waiting: false,
    busy_percent: None,
};

fn start_status() {
    let now = crate::monitor::ticks();
    unsafe {
        let u = &mut *addr_of_mut!(USAGE);
        core::arch::asm!("mrs {}, cntfrq_el0", out(reg) u.frequency);
        u.start = now;
        u.last = now;
    }
    draw_status(None);
}
pub fn waiting(value: bool) {
    sample();
    unsafe {
        (*addr_of_mut!(USAGE)).waiting = value;
    }
}
pub fn sample() {
    let u = unsafe { &mut *addr_of_mut!(USAGE) };
    let now = crate::monitor::ticks();
    if u.waiting {
        u.idle += now.wrapping_sub(u.last);
    }
    u.last = now;
    let elapsed = now.wrapping_sub(u.start);
    if elapsed < u.frequency || elapsed == 0 {
        return;
    }
    let busy = (elapsed.saturating_sub(u.idle) * 100 + elapsed / 2) / elapsed;
    u.start = now;
    u.idle = 0;
    u.busy_percent = Some(busy);
    draw_status(u.busy_percent);
}

pub fn cpu_busy_percent() -> Option<u64> {
    unsafe { (*addr_of!(USAGE)).busy_percent }
}

fn draw_status(cpu: Option<u64>) {
    const SURFACE: u32 = 0x273038;
    let (heap, _) = crate::monitor::heap_usage();
    // The kernel span includes its heap arena. Count only the allocated
    // portion, plus the separate physical region reserved for EL0 programs.
    let kernel_span = crate::monitor::kernel_span_bytes();
    let used = kernel_span - crate::HEAP_BYTES
        + heap
        + crate::user::BANKS * crate::user::USER_REGION_BYTES;
    let tenths_mib = (used * 10 + (1 << 19)) >> 20;
    let cpu = cpu.map_or_else(
        || alloc::string::String::from("--"),
        |n| alloc::format!("{n}%"),
    );
    let metrics = alloc::format!(
        "CPU {cpu:>4}    RAM {}.{} / 128 MiB    HEAP {} KiB",
        tenths_mib / 10,
        tenths_mib % 10,
        heap.div_ceil(1024)
    );
    let fb = unsafe { &mut (*addr_of_mut!(FB)).0 };
    let bar = &mut fb[..BAR_PIXELS];
    bar.fill(SURFACE);
    for (col, text, ink) in [
        (2, "LixinOS", 0xa9d5cd),
        (SCREEN_COLS - 2 - metrics.len(), metrics.as_str(), FG),
    ] {
        for (i, byte) in text.bytes().enumerate() {
            glyph(bar, (col + i) * CW, byte, ink, SURFACE);
        }
    }
    #[cfg(feature = "split")]
    focus_label(bar);
}
