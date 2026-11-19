//! QEMU virt ramfb and keyboard. One CPU, interrupts masked throughout.
use crate::disk::Dma;
use core::ptr::{NonNull, addr_of, addr_of_mut, read_volatile, write_volatile};
use virtio_drivers::{device::input::VirtIOInput, transport::mmio::MmioTransport};

const CW: usize = 22;
const CH: usize = 44;
const PANES: usize = if cfg!(feature = "split") { 2 } else { 1 };
pub const COLS: usize = if cfg!(feature = "split") { 68 } else { 107 };
const ROWS: usize = 31;
const MAX_ROWS: usize = 2304 / CH - 1;
const MAX_CELLS: usize = 4096 / CW * MAX_ROWS;
// The surface can change size while its backing memory retains a fixed stride.
const W: usize = 4096;
const H: usize = 2304;
const BAR_PIXELS: usize = W * CH;
const BG: u32 = 0x171b20;
const FG: u32 = 0xdadbd5;
const STATUS_BG: u32 = 0x273038;
const STATUS_VALUE: u32 = 0xe8ece9;
const STATUS_DIVIDER: u32 = 0x536471;
const STATUS_CLOCK: u32 = 0xd4e3ff;
#[cfg(feature = "split")]
const WIDE_FOCUS: usize = 39 * CW;
const FONT: &[u8; 95 * CW * CH] = include_bytes!(concat!(env!("OUT_DIR"), "/font.gray"));
#[repr(align(4096))]
struct Frame([u32; W * H]);
static mut FB: Frame = Frame([0; W * H]);

fn display_config() -> (u16, i32) {
    let data = 0x0902_0000 as *const u8;
    let select = |id: u16| unsafe { write_volatile(0x0902_0008 as *mut u16, id.to_be()) };
    let read = || unsafe { read_volatile(data) };
    select(0x19);
    let count = u32::from_be_bytes(core::array::from_fn(|_| read()));
    let mut framebuffer = None;
    let mut clock = None;
    for _ in 0..count {
        let entry: [u8; 64] = core::array::from_fn(|_| read());
        let id = u16::from_be_bytes([entry[4], entry[5]]);
        if entry[8..].starts_with(b"etc/ramfb\0") {
            framebuffer = Some(id);
        } else if entry[8..].starts_with(b"opt/lixinos/clock-offset\0")
            && u32::from_be_bytes([entry[0], entry[1], entry[2], entry[3]]) >= 5
        {
            clock = Some(id);
        }
    }
    let offset = clock.map_or(0, |id| {
        select(id);
        let [sign, hours_hi, hours_lo, minutes_hi, minutes_lo] =
            core::array::from_fn(|_| read());
        let digits = [hours_hi, hours_lo, minutes_hi, minutes_lo];
        if !matches!(sign, b'+' | b'-') || !digits.iter().all(u8::is_ascii_digit) {
            return 0;
        }
        let [a, b, c, d] = digits.map(|digit| i32::from(digit - b'0'));
        let hours = a * 10 + b;
        let minutes = c * 10 + d;
        if hours >= 24 || minutes >= 60 { return 0; }
        (hours * 3600 + minutes * 60) * if sign == b'-' { -1 } else { 1 }
    });
    (framebuffer.expect("missing QEMU ramfb"), offset)
}

fn configure_ramfb(selector: u16, width: usize, height: usize) {
    let mut record = [0u8; 28];
    record[..8].copy_from_slice(&(addr_of!(FB) as u64).to_be_bytes());
    for (out, value) in record[8..].chunks_exact_mut(4).zip([
        u32::from_le_bytes(*b"XR24"),
        0,
        width as u32,
        height as u32,
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
    let mut transfer = Transfer {
        control: ((u32::from(selector) << 16) | 8 | 16).to_be(),
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
}

struct Terminal {
    cols: usize,
    rows: usize,
    bar: Option<top_bar::TopBar>,
    x: usize,
    y: usize,
    color: u32,
    background: u32,
    wrap_pending: bool,
    reverse: bool,
    parser: ansi::Parser,
    params: [usize; 8],
    param: usize,
    cursor_visible: bool,
    application_cursor: bool,
    auto_wrap: bool,
    margins: Option<(usize, usize)>,
    saved_cursor: (usize, usize),
    alternate: Option<display::Screen>,
    charset: [bool; 2],
    active_charset: usize,
    utf8: [u8; 4],
    utf8_used: usize,
    utf8_expected: usize,
    cells: [u8; MAX_CELLS],
    styles: alloc::vec::Vec<scrollback::Style>,
    history: scrollback::History,
    wrapped: [bool; MAX_ROWS],
    selection: Option<(usize, usize)>,
    selecting: bool,
}
static mut ACTIVE: usize = 0;
static mut CANVAS_OPEN: [bool; PANES] = [false; PANES];
const INITIAL: Terminal = Terminal {
    cols: COLS, rows: ROWS,
    bar: None,
    x: 0,
    y: 0,
    color: FG,
    background: BG,
    wrap_pending: false,
    reverse: false,
    parser: ansi::Parser::Ground,
    params: [0; 8],
    param: 0,
    cursor_visible: true,
    application_cursor: false,
    auto_wrap: true,
    margins: None,
    saved_cursor: (0, 0),
    alternate: None,
    charset: [false; 2],
    active_charset: 0,
    utf8: [0; 4],
    utf8_used: 0,
    utf8_expected: 0,
    cells: [b' '; MAX_CELLS],
    styles: alloc::vec::Vec::new(),
    history: scrollback::History::new(),
    wrapped: [false; MAX_ROWS],
    selection: None,
    selecting: false,
};
static mut TERMS: [Terminal; PANES] = [const { INITIAL }; PANES];

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
    fn draw(&mut self, fb: &mut [u32], byte: u8) {
        let (fg, bg) = if self.reverse {
            (self.background, self.color)
        } else {
            (self.color, self.background)
        };
        glyph(fb, self.y * CH * W + self.x * CW, CW, byte, fg, bg);
        self.cells[self.y * self.cols + self.x] = byte;
        self.styles[self.y * self.cols + self.x] = (fg,bg);
    }
    fn highlight(&self, fb: &mut [u32]) {
        let Some((anchor, end)) = self.selection else { return };
        for index in anchor.min(end)..=anchor.max(end) {
            let top = index / self.cols * CH;
            let left = index % self.cols * CW;
            for y in top..top + CH {
                let offset = y * W + left;
                for pixel in &mut fb[offset..offset + CW] { *pixel ^= 0x506070; }
            }
        }
    }
    fn clear_selection(&mut self, fb: &mut [u32]) {
        self.highlight(fb);
        self.selection = None;
        self.selecting = false;
    }
    fn select(&mut self, fb: &mut [u32], point: Option<(usize, usize)>, down: bool) {
        if !down {
            self.selecting = false;
            return;
        }
        let Some((x, y)) = point else { return };
        let index = y * self.cols + x;
        if self.selecting && self.selection.is_some_and(|(_, end)| end == index) { return; }
        self.highlight(fb);
        let anchor = self.selection.filter(|_| self.selecting).map_or(index, |(anchor, _)| anchor);
        self.selection = Some((anchor, index));
        self.selecting = true;
        self.highlight(fb);
    }
    fn selection_text(&self) -> alloc::vec::Vec<u8> {
        let Some((anchor, end)) = self.selection else { return alloc::vec::Vec::new() };
        let start = anchor.min(end);
        let end = anchor.max(end) + 1;
        let mut text = alloc::vec::Vec::new();
        for row in start / self.cols..=(end - 1) / self.cols {
            let line = row * self.cols;
            let (visible, _) = self.visible_row(row);
            let first = start.saturating_sub(line).min(visible.len());
            let last = (end-line).min(self.cols).min(visible.len());
            let cells = &visible[first..last];
            let length = cells.iter().rposition(|&byte| byte != b' ').map_or(0, |i| i + 1);
            if row != start / self.cols { text.push(b'\n'); }
            text.extend_from_slice(&cells[..length]);
        }
        text
    }
    fn byte(&mut self, fb: &mut [u32], byte: u8) {
        if self.selection.is_some() { self.clear_selection(fb); }
        self.styles.resize(self.cols*self.rows, scrollback::DEFAULT_STYLE);
        if self.history.offset != 0 {
            self.viewport(fb,0);
        } else {
            self.cursor(fb);
        }
        self.parse(fb, byte);
        self.cursor(fb);
    }
    fn cursor(&self, fb: &mut [u32]) {
        if !self.cursor_visible || self.history.offset != 0 { return; }
        let start = ((self.y + 1) * CH - 3) * W + self.x * CW;
        for p in &mut fb[start..start + CW] {
            *p ^= 0xb0b0b0;
        }
    }
}
pub fn put(byte: u8) {
    if crate::scheduler::background() {
        let id=popup::terminal_id();
        if id!=crate::scheduler::terminal_id() || (id==0 && unsafe {CANVAS_OPEN[ACTIVE]}) {return;}
    }
    sample();
    if popup::write_popup(byte) { return; }
    unsafe {
        let fb = &mut (*addr_of_mut!(FB)).0;
        let term = &mut TERMS[ACTIVE];
        let start = (1 + term.bar_rows()) * BAR_PIXELS + ACTIVE * (pane_width() + CW);
        term.byte(&mut fb[start..], byte);
    }
}

#[cfg(feature = "split")]
pub fn switch_pane() {
    sample();
    unsafe { ACTIVE ^= 1; }
}
#[cfg(feature = "split")]
pub fn focus_changed() {
    reset_pointer();
    unsafe { focus_label(&mut (&mut (*addr_of_mut!(FB)).0)[..BAR_PIXELS]); }
}
#[cfg(feature = "split")]
fn focus_label(bar: &mut [u32]) {
    let label = match (crate::scheduler::focus(), display::size().width >= WIDE_FOCUS) {
        (0, true) => "Left  | Ctrl+Tab",
        (_, true) => "Right | Ctrl+Tab",
        (0, false) => "Left ",
        (_, false) => "Right",
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
    display::init();
    unsafe {
        let fb = &mut (*addr_of_mut!(FB)).0;
        fb.fill(BG);
        TERMS[0].cursor(&mut fb[BAR_PIXELS..]);
        if PANES == 2 {
            TERMS[1].cursor(&mut fb[BAR_PIXELS + pane_width() + CW..]);
            for row in fb[BAR_PIXELS..].chunks_exact_mut(W) {
                row[pane_width() + CW / 2..pane_width() + CW / 2 + 2].fill(0x44525b);
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
    crate::clipboard::init();
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
        if ctrl && shift && matches!(e.code, 46 | 47) {
            if e.value == 1 {
                return Some(if e.code == 46 { crate::clipboard::COPY } else { crate::clipboard::PASTE });
            }
            continue;
        }
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
            59..=68 | 87 | 88 => {
                k.pending = match e.code {
                    59 => b"\x1bOP", 60 => b"\x1bOQ", 61 => b"\x1bOR", 62 => b"\x1bOS",
                    63 => b"\x1b[15~", 64 => b"\x1b[17~", 65 => b"\x1b[18~", 66 => b"\x1b[19~",
                    67 => b"\x1b[20~", 68 => b"\x1b[21~", 87 => b"\x1b[23~", 88 => b"\x1b[24~",
                    _ => unreachable!(),
                };
                continue;
            }
            102..=111 => {
                #[cfg(feature="split")]
                let target=crate::scheduler::focus();
                #[cfg(not(feature="split"))]
                let target=0;
                let application = popup::application_cursor(target).unwrap_or_else(|| unsafe {
                    TERMS[target].application_cursor
                });
                k.pending = match e.code {
                    102 if application => b"\x1bOH",
                    103 if application => b"\x1bOA",
                    105 if application => b"\x1bOD",
                    106 if application => b"\x1bOC",
                    107 if application => b"\x1bOF",
                    108 if application => b"\x1bOB",
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

struct Usage {
    frequency: u64,
    start: crate::monitor::CpuCounters,
    busy_percent: Option<u64>,
}
static mut USAGE: Usage = Usage {
    frequency: 0,
    start: crate::monitor::CpuCounters::ZERO,
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
    unsafe {
        let u = &mut *addr_of_mut!(USAGE);
        u.frequency = crate::monitor::seconds_to_ticks(1);
        u.start = crate::monitor::cpu_counters();
    }
    draw_status(None);
}
pub fn sample() {
    let u = unsafe { &mut *addr_of_mut!(USAGE) };
    let now = crate::monitor::cpu_counters();
    if now.ticks.wrapping_sub(u.start.ticks) < u.frequency {
        return;
    }
    u.busy_percent = now.busy_percent(u.start);
    u.start = now;
    draw_status(u.busy_percent);
}

fn draw_status(cpu: Option<u64>) {
    let stack = crate::monitor::kernel_stack_usage().map_or_else(|| "--".into(), |(used, total)| {
        let used = memory_value(used);
        let total = memory_value(total);
        let unit = if total.ends_with("MiB") { "MiB" } else { "KiB" };
        alloc::format!("{}/{total}", used.strip_suffix(unit).unwrap_or(&used))
    });
    let (heap, _) = crate::monitor::heap_usage();
    // User images and process stacks are charged to the heap.
    let kernel_span = crate::monitor::kernel_span_bytes();
    let used = kernel_span + heap;
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
            alloc::format!("{}.{}/{}MiB", tenths_mib / 10, tenths_mib % 10, crate::inspect::ram_size().unwrap_or(crate::MAPPED_RAM).min(crate::MAPPED_RAM) >> 20),
            0x77d5d2,
        ),
        ("HEAP", memory_value(heap), 0xc6aef2),
        ("STACK", stack, 0xe4b4aa),
        ("FILES", files, 0xa9d295),
        ("DISK", disk, 0xf0c47a),
    ];
    let viewport = display::size().width;
    #[cfg(feature = "split")]
    let left = if viewport >= WIDE_FOCUS { 30 * CW } else { 18 * CW };
    #[cfg(not(feature = "split"))]
    let left = 10 * CW;
    let seconds = (i64::from(crate::utility::wall_seconds()) + i64::from(display::clock_offset()))
        .rem_euclid(86_400);
    let hours = seconds / 3600;
    let minutes = seconds / 60 % 60;
    let clock = if viewport >= 26 * CW {
        alloc::format!("{hours:02}:{minutes:02}:{:02}", seconds % 60)
    } else {
        alloc::format!("{hours:02}:{minutes:02}")
    };
    let gap = CW;
    let clock_x = viewport - (clock.len() + 1) * CW;
    let right = clock_x - gap;
    let mut selected = 0;
    let mut occupied = 0;
    for (index, (label, value, _)) in metrics.iter().enumerate() {
        let extra = (label.len() + value.len() + 1) * CW + if selected != 0 { gap } else { 0 };
        if left + occupied + extra > right { continue; }
        selected |= 1 << index;
        occupied += extra;
    }
    let mut x = right - occupied;
    let fb = unsafe { &mut (*addr_of_mut!(FB)).0 };
    let bar = &mut fb[..BAR_PIXELS];
    bar.fill(STATUS_BG);
    if clock_x >= 10 * CW {
        status_text(bar, 2 * CW, CW, "LixinOS", 0xa9d5cd);
    }
    let mut divider_needed = false;
    for (index, (label, value, ink)) in metrics.iter().enumerate() {
        if (selected & (1 << index)) == 0 { continue; }
        if divider_needed {
            status_divider(bar, x);
            x += gap;
        }
        status_text(bar, x, CW, label, *ink);
        x += (label.len() + 1) * CW;
        status_text(bar, x, CW, value, STATUS_VALUE);
        x += value.len() * CW;
        divider_needed = true;
    }
    if divider_needed {
        status_divider(bar, right);
    }
    status_text(bar, clock_x, CW, &clock, STATUS_CLOCK);
    #[cfg(feature = "split")]
    focus_label(bar);
}

fn status_divider(bar: &mut [u32], x: usize) {
    let divider = x + CW / 2 - 1;
    for row in 12..CH - 12 {
        let start = row * W + divider;
        bar[start..start + 2].fill(STATUS_DIVIDER);
    }
}

fn status_text(bar: &mut [u32], x: usize, width: usize, text: &str, ink: u32) {
    for (i, byte) in text.bytes().enumerate() {
        glyph(bar, x + i * width, width, byte, ink, STATUS_BG);
    }
}

// QEMU virtio-tablet absolute axes range from 0 to 32767. Use a logical
// pixel for each 2x2 Retina block. The top status row and gutter stay private.
fn pane_width() -> usize { display::size().cols * CW }
fn canvas_w() -> usize { pane_width() / 2 }
fn canvas_h() -> usize { display::size().rows * CH / 2 }
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
                    let x = self.x as usize * (display::size().width - 1) / 32767;
                    let y = self.y as usize * (display::size().height - 1) / 32767;
                    let left = unsafe { ACTIVE } * (pane_width() + CW);
                    let inside = (left..left + pane_width()).contains(&x) && y >= CH;
                    let local_x = x.saturating_sub(left).min(pane_width() - 1) / 2;
                    let local_y = y.saturating_sub(CH).min(canvas_h() * 2 - 1) / 2;
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
    canvas_w() as u64 | ((canvas_h() as u64) << 16)
}
pub fn pointer_event() -> u64 {
    if crate::scheduler::background() {return u64::MAX;}
    #[cfg(feature = "split")]
    if crate::scheduler::pane() != crate::scheduler::focus() { return u64::MAX; }
    let Some(p) = (unsafe { (&mut *addr_of_mut!(POINTER)).as_mut() }) else {
        return u64::MAX;
    };
    if core::mem::take(&mut p.cancel) {
        return 1 << 28;
    }
    p.next().unwrap_or(u64::MAX)
}

fn poll_console_selection() {
    if unsafe { CANVAS_OPEN[ACTIVE] } || crate::foreground::canvas_active() { return; }
    loop {
        let event = pointer_event();
        if event == u64::MAX { return; }
        let down = event & (1 << 24) != 0;
        let inside = event & (1 << 25) != 0;
        unsafe {
            let fb = &mut (*addr_of_mut!(FB)).0;
            let term = &mut TERMS[ACTIVE];
            let start = (1 + term.bar_rows()) * BAR_PIXELS + ACTIVE * (pane_width() + CW);
            let pixels = &mut fb[start..];
            let row = ((event >> 12) & 4095) as usize * 2 / CH;
            let point = inside.then_some(row).and_then(|row| row.checked_sub(term.bar_rows()))
                .map(|row| (((event & 4095) as usize * 2) / CW, row));
            if inside { term.wheel(pixels, event); }
            term.select(pixels, point, down);
        }
    }
}

pub fn copied_text() -> alloc::vec::Vec<u8> {
    popup::popup_selection().unwrap_or_else(|| unsafe { TERMS[ACTIVE].selection_text() })
}
pub fn present(arena: &crate::user::UserArena) -> u64 {
    // The caller supplies a tightly packed XRGB rectangle in its own arena.
    let [address, x, y, w, h] = unsafe { crate::user::git_mailbox().cast::<[usize; 5]>().read() };
    if w == 0
        || h == 0
        || w > canvas_w()
        || h > canvas_h()
        || x > canvas_w() - w
        || y > canvas_h() - h
        || address % 4 != 0
        || !arena.contains(address, w * h * 4)
    {
        return u64::MAX;
    }
    unsafe {
        let pixels = core::slice::from_raw_parts(address as *const u32, w * h);
        let fb = &mut (*addr_of_mut!(FB)).0;
        let start = BAR_PIXELS + ACTIVE * (pane_width() + CW) + 2 * (y * W + x);
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
    underneath: bool,
}
impl Canvas {
    pub fn open() -> Option<Self> {
        if canvas_size() == 0 || canvas_w() < 460 || canvas_h() < 440 || popup_active() { return None; }
        let mut arena = crate::user::UserArena::new();
        arena.grow((canvas_w() * canvas_h() + 20 * 24) * 4).ok()?;
        let pane = unsafe { ACTIVE };
        unsafe {
            let fb = &mut (*addr_of_mut!(FB)).0;
            let term = &mut TERMS[pane];
            let start = (1 + term.bar_rows()) * BAR_PIXELS + pane * (pane_width() + CW);
            term.clear_selection(&mut fb[start..]);
        }
        let mut original = alloc::vec::Vec::new();
        original.try_reserve_exact(pane_width() * canvas_h() * 2).ok()?;
        let fb = unsafe { &(*addr_of!(FB)).0 };
        let x = pane * (pane_width() + CW);
        for row in fb[BAR_PIXELS..].chunks_exact(W).take(canvas_h() * 2) {
            original.extend_from_slice(&row[x..x + pane_width()]);
        }
        begin_canvas();
        let underneath = unsafe { core::mem::replace(&mut CANVAS_OPEN[pane], true) };
        Some(Self { arena, original, pane, underneath })
    }
}
impl Drop for Canvas {
    fn drop(&mut self) {
        unsafe { CANVAS_OPEN[self.pane] = self.underneath; }
        let fb = unsafe { &mut (*addr_of_mut!(FB)).0 };
        let x = self.pane * (pane_width() + CW);
        for (row, saved) in fb[BAR_PIXELS..].chunks_exact_mut(W).zip(self.original.chunks_exact(pane_width())) {
            row[x..x + pane_width()].copy_from_slice(saved);
        }
    }
}

mod scrollback;
mod ansi;
mod display;
pub(crate) use display::{apply_resize, request_resize};
mod popup;
pub use popup::*;
mod top_bar;
pub(crate) use top_bar::{PinnedBar, TopBar, pin_bar, update_bar};

// XOR a pointer only while the shell is waiting. Erase it before returning a
// key so scrolling or saving the window never copies the pointer into its text.
fn toggle_pointer(x: usize, y: usize) {
    let fb = unsafe { &mut (*addr_of_mut!(FB)).0 };
    let origin = BAR_PIXELS + unsafe { ACTIVE } * (pane_width()+CW);
    for j in 0..16.min(canvas_h()-y) {
        for i in 0..(1+j*2/3).min(canvas_w()-x) {
            let p = origin + 2*((y+j)*W+x+i);
            for offset in [0, 1, W, W+1] { fb[p+offset] ^= 0xffffff; }
        }
    }
}
