// SPDX-License-Identifier: GPL-3.0-only
use core::arch::asm;
use std::{cell::Cell, rc::Rc, time::Duration};

use slint::platform::{
    Key, Platform, PointerEventButton, WindowAdapter, WindowEvent,
    software_renderer::{
        MinimalSoftwareWindow, PremultipliedRgbaColor, RepaintBufferType, TargetPixel,
    },
};
use slint::{ComponentHandle, LogicalPosition, PhysicalSize};

slint::include_modules!();

const CANVAS_PIXELS: *mut Xrgb = 0x6800_0000 as *mut Xrgb;
const MAILBOX: *mut [usize; 6] = 0x443d_8000 as *mut [usize; 6];

fn call(operation: usize, value: usize) -> isize {
    let result: isize;
    // This program runs at EL0 under the LixinOS AArch64 syscall ABI.
    unsafe {
        asm!("svc #0", in("x8") operation, inlateout("x0") value => result, clobber_abi("C"));
    }
    result
}

#[repr(transparent)]
#[derive(Clone, Copy)]
struct Xrgb(u32);

impl TargetPixel for Xrgb {
    fn blend(&mut self, color: PremultipliedRgbaColor) {
        let a = u32::from(255 - color.alpha);
        let blend =
            |shift: u32, component: u8| ((self.0 >> shift & 255u32) * a / 255) as u8 + component;
        *self = Self::from_rgb(
            blend(16, color.red),
            blend(8, color.green),
            blend(0, color.blue),
        );
    }

    fn from_rgb(red: u8, green: u8, blue: u8) -> Self {
        Self((u32::from(red) << 16) | (u32::from(green) << 8) | u32::from(blue))
    }
}

struct GuestPlatform {
    window: Rc<MinimalSoftwareWindow>,
    start_ms: usize,
}

impl Platform for GuestPlatform {
    fn create_window_adapter(&self) -> Result<Rc<dyn WindowAdapter>, slint::PlatformError> {
        Ok(self.window.clone())
    }

    fn duration_since_start(&self) -> Duration {
        let now = call(38, 0) as usize;
        Duration::from_millis(now.saturating_sub(self.start_ms) as u64)
    }
}

#[derive(Default)]
struct Pointer {
    inside: bool,
    down: bool,
    position: Option<(usize, usize)>,
}

impl Pointer {
    fn dispatch(&mut self, window: &slint::Window, event: usize) {
        let inside = event & (1 << 25) != 0 && event & (1 << 28) == 0;
        if !inside {
            if self.inside {
                window.dispatch_event(WindowEvent::PointerExited);
            }
            self.inside = false;
            self.down = false;
            self.position = None;
            return;
        }
        let x = event & 4095;
        let y = (event >> 12) & 4095;
        self.position = Some((x, y));
        let position = LogicalPosition::new(x as f32, y as f32);
        window.dispatch_event(WindowEvent::PointerMoved { position });
        let down = event & (1 << 24) != 0;
        if down != self.down {
            let button = PointerEventButton::Left;
            window.dispatch_event(if down {
                WindowEvent::PointerPressed { position, button }
            } else {
                WindowEvent::PointerReleased { position, button }
            });
        }
        let wheel = ((event >> 26) & 1) as i32 - ((event >> 27) & 1) as i32;
        if wheel != 0 {
            window.dispatch_event(WindowEvent::PointerScrolled {
                position,
                delta_x: 0.0,
                delta_y: wheel as f32 * 32.0,
            });
        }
        self.inside = true;
        self.down = down;
    }
}

#[derive(PartialEq)]
enum KeyStroke {
    Escape,
    Byte(isize),
    Special(Key),
}

impl KeyStroke {
    fn dispatch(self, window: &slint::Window) {
        let text: slint::SharedString = match self {
            Self::Special(key) => key.into(),
            Self::Byte(8 | 127) => Key::Backspace.into(),
            Self::Byte(9) => Key::Tab.into(),
            Self::Byte(10 | 13) => Key::Return.into(),
            Self::Byte(byte @ 32..=126) => (byte as u8 as char).to_string().into(),
            _ => return,
        };
        window.dispatch_event(WindowEvent::KeyPressed { text: text.clone() });
        window.dispatch_event(WindowEvent::KeyReleased { text });
    }
}

#[derive(Default)]
struct Keyboard {
    sequence: [u8; 8],
    len: usize,
    deadline: usize,
}

impl Keyboard {
    fn decode(&mut self, byte: isize) -> Option<KeyStroke> {
        if self.len == 0 {
            if byte == 27 {
                self.sequence[0] = 27;
                self.len = 1;
                self.deadline = call(38, 0).max(0) as usize + 35;
                return None;
            }
            return (0..256).contains(&byte).then_some(KeyStroke::Byte(byte));
        }
        if !(0..256).contains(&byte) {
            if (call(38, 0).max(0) as usize) < self.deadline {
                return None;
            }
            let escape = self.len == 1;
            self.len = 0;
            return escape.then_some(KeyStroke::Escape);
        }

        let byte = byte as u8;
        self.sequence[self.len] = byte;
        self.len += 1;
        if self.len == 2 && matches!(byte, b'[' | b'O') {
            return None;
        }
        if self.len > 2 && self.len < self.sequence.len() && (b'0'..=b'?').contains(&byte) {
            return None;
        }
        let key = match &self.sequence[1..self.len] {
            [b'[' | b'O', b'A'] => Key::UpArrow,
            [b'[' | b'O', b'B'] => Key::DownArrow,
            [b'[' | b'O', b'C'] => Key::RightArrow,
            [b'[' | b'O', b'D'] => Key::LeftArrow,
            [b'[' | b'O', b'H'] | b"[1~" | b"[7~" => Key::Home,
            [b'[' | b'O', b'F'] | b"[4~" | b"[8~" => Key::End,
            b"[3~" => Key::Delete,
            b"[5~" => Key::PageUp,
            b"[6~" => Key::PageDown,
            _ => {
                self.len = 0;
                return None;
            }
        };
        self.len = 0;
        Some(KeyStroke::Special(key))
    }
}

#[derive(Clone, Copy, PartialEq)]
struct Canvas {
    width: usize,
    height: usize,
    generation: usize,
}

impl Canvas {
    fn query() -> Result<Self, String> {
        let size = call(19, 1);
        if size <= 0 {
            return Err("a foreground QEMU canvas is required".into());
        }
        Ok(Self {
            width: size as usize & 65535,
            height: (size as usize >> 16) & 65535,
            generation: size as usize >> 32,
        })
    }

    fn submit(self, x: usize, y: usize, w: usize, h: usize) -> isize {
        // The pixel pointer and rectangle stay in the kernel-owned canvas arena.
        let first = unsafe { CANVAS_PIXELS.add(y * self.width + x) };
        unsafe {
            MAILBOX.write([first as usize, x, y, w, h, self.width]);
        }
        call(20, self.generation)
    }

    fn cursor_tile(self, (x, y): (usize, usize), draw: bool) {
        if x >= self.width || y >= self.height {
            return;
        }
        let width = (self.width - x).min(9);
        let height = (self.height - y).min(14);
        let tile = unsafe { CANVAS_PIXELS.add(self.width * self.height) };
        for row in 0..height {
            for col in 0..width {
                let base = unsafe { CANVAS_PIXELS.add((y + row) * self.width + x + col).read() };
                let color = if draw && col <= row / 2 && row < 13 {
                    Xrgb(if col == 0 || col == row / 2 || row == 12 { 0x101419 } else { 0xffffff })
                } else {
                    base
                };
                unsafe { tile.add(row * width + col).write(color); }
            }
        }
        unsafe {
            MAILBOX.write([tile as usize, x, y, width, height, width]);
        }
        call(20, self.generation);
    }

    fn set_size(self, adapter: &MinimalSoftwareWindow) {
        adapter.set_size(PhysicalSize::new(self.width as u32, self.height as u32));
    }
}

#[derive(Default)]
pub struct SubmissionStats {
    pub full_frames: usize,
    pub partial_frames: usize,
    pub largest_partial: usize,
}

pub fn run<T: ComponentHandle>(
    setup: impl FnOnce(Rc<Cell<bool>>) -> Result<T, String>,
    mut shortcut: impl FnMut(&T, isize) -> bool,
    cursor: bool,
) -> Result<SubmissionStats, String> {
    let mut canvas = Canvas::query()?;
    let mut stats = SubmissionStats::default();
    let adapter = MinimalSoftwareWindow::new(if canvas.generation == 0 {
        RepaintBufferType::NewBuffer
    } else {
        RepaintBufferType::ReusedBuffer
    });
    slint::platform::set_platform(Box::new(GuestPlatform {
        window: adapter.clone(),
        start_ms: call(38, 0).max(0) as usize,
    }))
    .map_err(|error| format!("{error:?}"))?;
    let closed = Rc::new(Cell::new(false));
    let component = setup(closed.clone())?;
    component.show().map_err(|error| format!("{error:?}"))?;
    let window = component.window();
    window.dispatch_event(WindowEvent::ScaleFactorChanged { scale_factor: 1.0 });
    canvas.set_size(&adapter);
    let mut pointer = Pointer::default();
    let mut full_redraw = true;
    let mut drawn_pointer = None;
    let mut keyboard = Keyboard::default();
    while !closed.get() {
        let current = Canvas::query()?;
        if current != canvas {
            canvas = current;
            drawn_pointer = None;
            canvas.set_size(&adapter);
            pointer.dispatch(window, 1 << 28);
            adapter.request_redraw();
            full_redraw = true;
        }
        slint::platform::update_timers_and_animations();
        for _ in 0..64 {
            let event = call(21, 0);
            if event < 0 {
                break;
            }
            pointer.dispatch(window, event as usize);
        }
        if closed.get() {
            break;
        }
        let next_pointer = if cursor { pointer.position } else { None };
        let moved = next_pointer != drawn_pointer;
        if moved {
            if let Some(previous) = drawn_pointer {
                canvas.cursor_tile(previous, false);
            }
        }
        let mut rendered = false;
        let mut status = 0;
        adapter.draw_if_needed(|renderer| {
            // Re-read the arena length after resize and keep the slice local to this frame.
            let pixels = unsafe {
                std::slice::from_raw_parts_mut(CANVAS_PIXELS, canvas.width * canvas.height)
            };
            if full_redraw && canvas.generation != 0 {
                renderer.set_repaint_buffer_type(RepaintBufferType::NewBuffer);
                renderer.set_repaint_buffer_type(RepaintBufferType::ReusedBuffer);
            }
            let dirty = renderer.render(pixels, canvas.width);
            rendered = true;
            if full_redraw || canvas.generation == 0 {
                status = canvas.submit(0, 0, canvas.width, canvas.height);
                stats.full_frames += usize::from(status == 0);
            } else {
                let mut count = 0;
                for (origin, size) in dirty.iter() {
                    status = canvas.submit(
                        origin.x as usize,
                        origin.y as usize,
                        size.width as usize,
                        size.height as usize,
                    );
                    if status != 0 {
                        break;
                    }
                    count += size.width as usize * size.height as usize;
                }
                if count > 0 && status == 0 {
                    if count >= canvas.width * canvas.height {
                        stats.full_frames += 1;
                    } else {
                        stats.partial_frames += 1;
                        stats.largest_partial = stats.largest_partial.max(count);
                    }
                }
            }
            full_redraw = status == -2;
        });
        if status < 0 && status != -2 {
            return Err("canvas submission failed".into());
        }
        if moved || rendered {
            if let Some(point) = next_pointer {
                canvas.cursor_tile(point, true);
            }
            drawn_pointer = next_pointer;
        }
        let key = call(14, if window.has_active_animations() { 8 } else { 16 });
        if let Some(key) = keyboard.decode(key) {
            if key == KeyStroke::Escape {
                if shortcut(&component, 27) {
                    continue;
                }
                break;
            }
            if let KeyStroke::Byte(byte) = key {
                if shortcut(&component, byte) {
                    continue;
                }
            }
            key.dispatch(window);
        }
    }
    component.hide().map_err(|error| format!("{error:?}"))?;
    Ok(stats)
}
