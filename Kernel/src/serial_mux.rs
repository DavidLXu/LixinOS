// SSH mode only. Host input frames: opcode, terminal u32 LE, length u16 LE, data.
// Guest output is a byte stream tagged with a terminal when the owner changes.
use alloc::collections::VecDeque;
use core::cell::UnsafeCell;

struct Frame {
    id: u32,
    data: VecDeque<u8>, // Empty only for an OPEN event.
}
struct Mux {
    active: u32,
    frames: VecDeque<Frame>,
    incoming: VecDeque<u8>,
}
struct Slot(UnsafeCell<Mux>);
// Single CPU, no split-pane switches in the serial SSH profile.
unsafe impl Sync for Slot {}
static SLOT: Slot = Slot(UnsafeCell::new(Mux {
    active: 1,
    frames: VecDeque::new(),
    incoming: VecDeque::new(),
}));
const OPEN_BIT: u32 = 1 << 30;

fn control(op: u8, id: u32) {
    for byte in [0xff, op].into_iter().chain(id.to_le_bytes()) {
        crate::uart_put(byte);
    }
}

pub fn write(byte: u8) {
    crate::uart_put(byte);
    if byte == 0xff {
        crate::uart_put(0);
    }
}

impl Mux {
    // Incomplete UART frames stay buffered. Yield only outside the mux borrow.
    fn pump(&mut self) {
        while let Some(byte) = crate::uart_poll() {
            self.incoming.push_back(byte);
            if self.incoming.len() < 7 {
                continue;
            }
            let len = u16::from_le_bytes([self.incoming[5], self.incoming[6]]) as usize;
            if self.incoming.len() < 7 + len {
                continue;
            }
            let op = self.incoming[0];
            let id = u32::from_le_bytes(core::array::from_fn(|i| self.incoming[i + 1]));
            self.incoming.drain(..7);
            if (1..OPEN_BIT).contains(&id)
                && ((op == 1 && len == 0) || (op == 2 && (1..=4096).contains(&len)))
            {
                self.frames.push_back(Frame {
                    id,
                    data: self.incoming.drain(..len).collect(),
                });
            } else {
                self.incoming.clear();
            }
            break;
        }
    }
}
pub fn next() -> u32 {
    let mux = loop {
        let mux = unsafe { &mut *SLOT.0.get() };
        if mux.frames.is_empty() {
            mux.pump();
        }
        if !mux.frames.is_empty() {
            break mux;
        }
        crate::scheduler::sleep_ms(1);
    };
    let first = mux.frames.front().unwrap();
    if mux.active != first.id {
        mux.active = first.id;
        control(1, mux.active);
    }
    if first.data.is_empty() {
        mux.frames.pop_front();
        mux.active | OPEN_BIT
    } else {
        mux.active
    }
}

pub fn poll() -> Option<u8> {
    let mux = unsafe { &mut *SLOT.0.get() };
    loop {
        if let Some(i) = mux
            .frames
            .iter()
            .position(|f| f.id == mux.active && !f.data.is_empty())
        {
            let byte = mux.frames[i].data.pop_front();
            if mux.frames[i].data.is_empty() {
                mux.frames.remove(i);
                control(2, mux.active); // Allow one more frame from this client.
            }
            return byte;
        }
        let before = mux.frames.len();
        mux.pump();
        if before == mux.frames.len() {
            return None;
        }
    }
}

pub fn close() {
    let mux = unsafe { &mut *SLOT.0.get() };
    mux.frames.retain(|frame| frame.id != mux.active);
    control(3, mux.active);
}
