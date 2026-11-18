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
const STATUS_BG: u32 = 0x273038;
const STATUS_VALUE: u32 = 0xe8ece9;
const STATUS_DIVIDER: u32 = 0x536471;
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
    cols: usize,
    rows: usize,
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
static mut CANVAS_OPEN: [bool; PANES] = [false; PANES];
const INITIAL: Terminal = Terminal {
    cols: COLS, rows: ROWS,
    x: 0,
    y: 0,
    color: FG,
    wrap_pending: false,
    reverse: false,
    escape: false,
    csi: false,
    params: [0; 4],
    param: 0,
};
static mut TERMS: [Terminal; PANES] = [INITIAL; PANES];

#[inline]
fn glyph(fb: &mut [u32], offset: usize, width: usize, byte: u8, fg: u32, bg: u32) {
    let glyph = &FONT[(byte - 32) as usize * CW * CH..];
    for row in 0..CH {
        let start = offset + row * W;
        for (x, p) in fb[start..start + width].iter_mut().enumerate() {
            let a = glyph[row * CW + x * CW / width] as u32;
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
        glyph(fb, self.y * CH * W + self.x * CW, CW, byte, fg, bg);
    }
    fn newline(&mut self, fb: &mut [u32]) {
        if self.y + 1 < self.rows {
            self.y += 1;
            return;
        }
        for y in 0..(self.rows - 1) * CH {
            let src = (y + CH) * W;
            fb.copy_within(src..src + self.cols * CW, y * W);
        }
        for y in (self.rows-1)*CH..self.rows*CH {
            fb[y*W..y*W+self.cols*CW].fill(BG);
        }
    }
    fn control(&mut self, fb: &mut [u32], byte: u8) {
        if byte != b'm' {
            self.wrap_pending = false;
        }
        let n = self.params[0].max(1);
        match byte {
            b'H' | b'f' => {
                self.y = (n - 1).min(self.rows - 1);
                self.x = self.params[1].saturating_sub(1).min(self.cols - 1);
            }
            b'A' => self.y = self.y.saturating_sub(n),
            b'B' => self.y = (self.y + n).min(self.rows - 1),
            b'C' => self.x = (self.x + n).min(self.cols - 1),
            b'D' => self.x = self.x.saturating_sub(n),
            b'J' if self.params[0] == 2 => {
                for row in fb.chunks_mut(W).take(self.rows * CH) {
                    row[..(self.cols * CW)].fill(BG);
                }
            }
            b'K' => {
                for row in self.y * CH..(self.y + 1) * CH {
                    fb[row * W + self.x * CW..row * W + (self.cols * CW)].fill(BG);
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
                        30 => self.color = 0x565e6b,
                        31 => self.color = 0xe69a96,
                        33 => self.color = 0xe7cc91,
                        34 => self.color = 0x90b7ed,
                        35 => self.color = 0xcdabd9,
                        37 | 39 => self.color = FG,
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
                9 => self.x = ((self.x / 8 + 1) * 8).min(self.cols - 1),
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
                    self.wrap_pending = self.x + 1 == self.cols;
                    self.x = (self.x + 1).min(self.cols - 1);
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
        if let Some(term) = &mut POPUPS[ACTIVE] {
            term.byte(&mut fb[popup_offset()..], byte);
        } else {
            TERMS[ACTIVE].byte(&mut fb[BAR_PIXELS + ACTIVE * (PANE_WIDTH + CW)..], byte);
        }
    }
}

#[cfg(feature = "split")]
pub fn switch_pane() {
    sample();
    unsafe {
        let fb = &mut (*addr_of_mut!(FB)).0;
        if !CANVAS_OPEN[ACTIVE] {
            TERMS[ACTIVE].cursor(&mut fb[BAR_PIXELS + ACTIVE * (PANE_WIDTH + CW)..]);
        }
        // Each suspended continuation may be waiting for input or doing work.
        static mut IDLE: [bool; 2] = [false; 2];
        IDLE[ACTIVE] = USAGE.waiting;
        ACTIVE ^= 1;
        reset_pointer();
        USAGE.waiting = IDLE[ACTIVE];
        if !CANVAS_OPEN[ACTIVE] {
            TERMS[ACTIVE].cursor(&mut fb[BAR_PIXELS + ACTIVE * (PANE_WIDTH + CW)..]);
        }
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
    status_text(bar, 12 * CW, CW, label, 0xcbdedb);
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
        init_tablet();
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

#[derive(Clone, Copy)]
struct StorageUsage {
    files: usize,
    used_bytes: usize,
    capacity_bytes: usize,
}
static mut STORAGE: Option<StorageUsage> = None;

pub fn set_storage_usage(files: usize, used_bytes: usize, capacity_bytes: usize) {
    // The disk and status sampler both run on the guest's single vCPU.
    unsafe {
        *addr_of_mut!(STORAGE) = Some(StorageUsage {
            files,
            used_bytes,
            capacity_bytes,
        });
    }
}

fn memory_value(bytes: usize) -> alloc::string::String {
    const MIB: usize = 1 << 20;
    if bytes < MIB {
        return alloc::format!("{}KiB", bytes.div_ceil(1024));
    }
    let tenths = (bytes * 10 + MIB / 2) / MIB;
    if tenths % 10 == 0 {
        alloc::format!("{}MiB", tenths / 10)
    } else {
        alloc::format!("{}.{}MiB", tenths / 10, tenths % 10)
    }
}

fn disk_value(usage: StorageUsage) -> alloc::string::String {
    const MIB: usize = 1024 * 1024;
    let used_kib = usage.used_bytes.div_ceil(1024);
    let capacity_kib = usage.capacity_bytes.div_ceil(1024);
    let capacity_mib = usage.capacity_bytes.div_ceil(MIB);
    if capacity_mib < 8 {
        alloc::format!("{used_kib}/{capacity_kib}KiB")
    } else if usage.used_bytes < MIB {
        alloc::format!("{used_kib}KiB/{capacity_mib}MiB")
    } else {
        let tenths = (usage.used_bytes * 10 + MIB / 2) / MIB;
        alloc::format!("{}.{}/{capacity_mib}MiB", tenths / 10, tenths % 10)
    }
}

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
    let stack = crate::monitor::kernel_stack_usage().map_or_else(|| "--".into(), |(used, total)| {
        let used = memory_value(used);
        let total = memory_value(total);
        let unit = if total.ends_with("MiB") { "MiB" } else { "KiB" };
        alloc::format!("{}/{total}", used.strip_suffix(unit).unwrap_or(&used))
    });
    let (heap, _) = crate::monitor::heap_usage();
    // The heap and user banks are outside the linked kernel image.
    let kernel_span = crate::monitor::kernel_span_bytes();
    let used = kernel_span
        + heap
        + crate::user::BANKS * crate::user::USER_REGION_BYTES;
    let tenths_mib = (used * 10 + (1 << 19)) >> 20;
    let cpu = cpu.map_or_else(
        || alloc::string::String::from("--"),
        |n| alloc::format!("{n}%"),
    );
    let storage = unsafe { *addr_of!(STORAGE) };
    let files = storage.map_or_else(|| "--".into(), |usage| alloc::format!("{}", usage.files));
    let disk = storage.map_or_else(|| "--".into(), disk_value);
    let metrics = [
        ("CPU", cpu, 0x8fbef4),
        (
            "RAM",
            alloc::format!("{}.{}/512MiB", tenths_mib / 10, tenths_mib % 10),
            0x77d5d2,
        ),
        ("HEAP", memory_value(heap), 0xc6aef2),
        ("STACK", stack, 0xe4b4aa),
        ("FILES", files, 0xa9d295),
        ("DISK", disk, 0xf0c47a),
    ];
    let characters: usize = metrics
        .iter()
        .map(|(label, value, _)| label.len() + 1 + value.len())
        .sum();
    let separators = metrics.len() - 1;
    let left = if cfg!(feature = "split") { 30 * CW } else { 10 * CW };
    let right = W - CW;
    // Fit the counters into the space after the title and pane selector.
    let width = CW.min((right - left - 12 * separators) / characters);
    let text_width = characters * width;
    let gap = (right.saturating_sub(left + text_width) / separators).clamp(12, 40);
    let mut x = right - text_width - separators * gap;
    let fb = unsafe { &mut (*addr_of_mut!(FB)).0 };
    let bar = &mut fb[..BAR_PIXELS];
    bar.fill(STATUS_BG);
    let title_x = if x >= 10 * CW { 2 * CW } else { 0 };
    status_text(bar, title_x, CW, "LixinOS", 0xa9d5cd);
    for (index, (label, value, ink)) in metrics.iter().enumerate() {
        status_text(bar, x, width, label, *ink);
        x += (label.len() + 1) * width;
        status_text(bar, x, width, value, STATUS_VALUE);
        x += value.len() * width;
        if index < separators {
            let divider = x + gap / 2 - 1;
            for row in 12..CH - 12 {
                let start = row * W + divider;
                bar[start..start + 2].fill(STATUS_DIVIDER);
            }
            x += gap;
        }
    }
    #[cfg(feature = "split")]
    focus_label(bar);
}

fn status_text(bar: &mut [u32], x: usize, width: usize, text: &str, ink: u32) {
    for (i, byte) in text.bytes().enumerate() {
        glyph(bar, x + i * width, width, byte, ink, STATUS_BG);
    }
}

// QEMU virtio-tablet absolute axes range from 0 to 32767. Use a logical
// pixel for each 2x2 Retina block. The top status row and gutter stay private.
const CANVAS_W: usize = PANE_WIDTH / 2;
const CANVAS_H: usize = ROWS * CH / 2;
struct Pointer {
    device: VirtIOInput<Dma, MmioTransport<'static>>,
    x: u32,
    y: u32,
    left: bool,
    wheel: i32,
    suppress: bool,
    cancel: bool,
}
static mut POINTER: Option<Pointer> = None;
fn init_tablet() {
    unsafe {
        let Some(device) = MmioTransport::new(NonNull::new(0x0a00_0600 as *mut _).unwrap(), 512)
            .ok()
            .and_then(|t| VirtIOInput::new(t).ok())
        else {
            return;
        };
        *addr_of_mut!(POINTER) = Some(Pointer {
            device,
            x: 0,
            y: 0,
            left: false,
            wheel: 0,
            suppress: false,
            cancel: false,
        });
    }
}
impl Pointer {
    fn next(&mut self) -> Option<u64> {
        while let Some(e) = self.device.pop_pending_event() {
            match (e.event_type, e.code) {
                (3, 0) => self.x = e.value.min(32767),
                (3, 1) => self.y = e.value.min(32767),
                (1, 272) => {
                    self.left = e.value != 0;
                    if !self.left {
                        self.suppress = false;
                    }
                }
                (2, 8) => self.wheel = (e.value as i32).signum(),
                (0, 0) => {
                    let x = self.x as usize * (W - 1) / 32767;
                    let y = self.y as usize * (H - 1) / 32767;
                    let left = unsafe { ACTIVE } * (PANE_WIDTH + CW);
                    let inside = (left..left + PANE_WIDTH).contains(&x) && y >= CH;
                    let local_x = x.saturating_sub(left).min(PANE_WIDTH - 1) / 2;
                    let local_y = y.saturating_sub(CH).min(ROWS * CH - 1) / 2;
                    let wheel = core::mem::take(&mut self.wheel);
                    return Some(
                        local_x as u64
                            | ((local_y as u64) << 12)
                            | (u64::from(self.left && !self.suppress) << 24)
                            | (u64::from(inside) << 25)
                            | (u64::from(wheel > 0) << 26)
                            | (u64::from(wheel < 0) << 27),
                    );
                }
                _ => (),
            }
        }
        None
    }
}
fn reset_pointer() {
    if let Some(p) = unsafe { (&mut *addr_of_mut!(POINTER)).as_mut() } {
        while p.next().is_some() {}
        p.suppress = p.left;
        p.cancel = true;
    }
}
pub fn begin_canvas() {
    reset_pointer();
}
pub fn canvas_size() -> u64 {
    if unsafe { (&*addr_of!(POINTER)).is_none() } {
        return 0;
    }
    CANVAS_W as u64 | ((CANVAS_H as u64) << 16)
}
pub fn pointer_event() -> u64 {
    let Some(p) = (unsafe { (&mut *addr_of_mut!(POINTER)).as_mut() }) else {
        return u64::MAX;
    };
    if core::mem::take(&mut p.cancel) {
        return 1 << 28;
    }
    p.next().unwrap_or(u64::MAX)
}
pub fn present(arena: &crate::user::UserArena) -> u64 {
    // The caller supplies a tightly packed XRGB rectangle in its own arena.
    let [address, x, y, w, h] = unsafe { crate::user::git_mailbox().cast::<[usize; 5]>().read() };
    if w == 0
        || h == 0
        || w > CANVAS_W
        || h > CANVAS_H
        || x > CANVAS_W - w
        || y > CANVAS_H - h
        || address % 4 != 0
        || !arena.contains(address, w * h * 4)
    {
        return u64::MAX;
    }
    unsafe {
        let pixels = core::slice::from_raw_parts(address as *const u32, w * h);
        let fb = &mut (*addr_of_mut!(FB)).0;
        let start = BAR_PIXELS + ACTIVE * (PANE_WIDTH + CW) + 2 * (y * W + x);
        for (row, src) in pixels.chunks_exact(w).enumerate() {
            let offset = start + 2 * row * W;
            for (pair, color) in fb[offset..offset + 2 * w].chunks_exact_mut(2).zip(src) {
                pair.fill(*color);
            }
            fb.copy_within(offset..offset + 2 * w, offset + W);
        }
    }
    0
}

// A synchronous graphics utility owns only its pane. Keep the original pixels
// so returning (including a user fault) restores the exact shell screen.
pub struct Canvas {
    pub arena: crate::user::UserArena,
    original: alloc::vec::Vec<u32>,
    pane: usize,
}
impl Canvas {
    pub fn open() -> Option<Self> {
        if canvas_size() == 0 || popup_active() { return None; }
        let mut arena = crate::user::UserArena::new();
        arena.grow((CANVAS_W * CANVAS_H + 20 * 24) * 4).ok()?;
        let pane = unsafe { ACTIVE };
        let mut original = alloc::vec::Vec::new();
        original.try_reserve_exact(PANE_WIDTH * CH * ROWS).ok()?;
        let fb = unsafe { &(*addr_of!(FB)).0 };
        let x = pane * (PANE_WIDTH + CW);
        for row in fb[BAR_PIXELS..].chunks_exact(W) {
            original.extend_from_slice(&row[x..x + PANE_WIDTH]);
        }
        begin_canvas();
        unsafe { CANVAS_OPEN[pane] = true; }
        Some(Self { arena, original, pane })
    }
}
impl Drop for Canvas {
    fn drop(&mut self) {
        unsafe { CANVAS_OPEN[self.pane] = false; }
        let fb = unsafe { &mut (*addr_of_mut!(FB)).0 };
        let x = self.pane * (PANE_WIDTH + CW);
        for (row, saved) in fb[BAR_PIXELS..].chunks_exact_mut(W).zip(self.original.chunks_exact(PANE_WIDTH)) {
            row[x..x + PANE_WIDTH].copy_from_slice(saved);
        }
    }
}

// Popup chrome is painted by the desktop. Only this character grid receives ANSI output.
static mut POPUPS: [Option<Terminal>; PANES] = [None; PANES];
const POP_COLS: usize = COLS - 4;
const POP_ROWS: usize = 21;
fn popup_offset() -> usize {
    BAR_PIXELS + unsafe { ACTIVE } * (PANE_WIDTH + CW) + 2 * CW + 3 * CH * W
}
pub fn columns() -> usize { if popup_active() { POP_COLS } else { COLS } }
pub fn rows() -> usize { if popup_active() { POP_ROWS } else { ROWS } }
pub fn popup_active() -> bool { unsafe { POPUPS[ACTIVE].is_some() } }

pub struct PopupScreen(Terminal, alloc::vec::Vec<u32>);
pub fn show_popup(saved: Option<PopupScreen>) {
    reset_pointer();
    let fb = unsafe { &mut (&mut (*addr_of_mut!(FB)).0)[popup_offset()..] };
    let (term, pixels) = match saved {
        Some(PopupScreen(t, p)) => (t, Some(p)),
        None => (Terminal {cols: POP_COLS, rows: POP_ROWS, ..INITIAL}, None)
    };
    for (y,row) in fb.chunks_mut(W).take(POP_ROWS*CH).enumerate() {
        let width = POP_COLS*CW;
        match &pixels {
            Some(p) => row[..width].copy_from_slice(&p[y*width..(y+1)*width]),
            None => row[..width].fill(BG)
        }
    }
    if pixels.is_none() { term.cursor(fb); }
    unsafe { POPUPS[ACTIVE] = Some(term); }
}
pub fn hide_popup(save: bool) -> Option<PopupScreen> {
    let term = unsafe { POPUPS[ACTIVE].take()? };
    if !save { return None; }
    let fb = unsafe { &(&(*addr_of!(FB)).0)[popup_offset()..] };
    let mut pixels = alloc::vec::Vec::with_capacity(POP_COLS*CW*POP_ROWS*CH);
    for row in fb.chunks(W).take(POP_ROWS*CH) {
        pixels.extend_from_slice(&row[..POP_COLS*CW]);
    }
    Some(PopupScreen(term, pixels))
}

// XOR a pointer only while the shell is waiting. Erase it before returning a
// key so scrolling or saving the window never copies the pointer into its text.
fn toggle_pointer(x: usize, y: usize) {
    let fb = unsafe { &mut (*addr_of_mut!(FB)).0 };
    let origin = BAR_PIXELS + unsafe { ACTIVE } * (PANE_WIDTH+CW);
    for j in 0..16.min(CANVAS_H-y) {
        for i in 0..(1+j*2/3).min(CANVAS_W-x) {
            let p = origin + 2*((y+j)*W+x+i);
            for offset in [0, 1, W, W+1] { fb[p+offset] ^= 0xffffff; }
        }
    }
}
pub fn popup_key() -> u64 {
    let mut cursor = None;
    waiting(true);
    let key = loop {
        if let Some(c) = crate::poll_byte() {
            break if c==29 {257} else {u64::from(c)};
        }
        let event = pointer_event();
        if event == u64::MAX {continue;}
        if let Some((x,y)) = cursor.take() {toggle_pointer(x,y);}
        if event & (1<<25) == 0 {continue;}
        let (x,y) = ((event & 4095) as usize, ((event>>12)&4095) as usize);
        if event & (1<<24) != 0 && (16..CANVAS_W-16).contains(&x) && (32..66).contains(&y) {
            break 257;
        }
        toggle_pointer(x,y);
        cursor=Some((x,y));
    };
    if let Some((x,y))=cursor { toggle_pointer(x,y); }
    waiting(false);
    key
}
