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
}
struct Slot(UnsafeCell<Mux>);
// Single CPU, no split-pane switches in the serial SSH profile.
unsafe impl Sync for Slot {}
static SLOT: Slot = Slot(UnsafeCell::new(Mux {
    active: 1,
    frames: VecDeque::new(),
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

fn read() -> u8 {
    loop {
        if let Some(b) = crate::uart_poll() {
            return b;
        }
        core::hint::spin_loop();
    }
}

impl Mux {
    fn receive(&mut self, op: u8) {
        let id = u32::from_le_bytes(core::array::from_fn(|_| read()));
        let len = u16::from_le_bytes([read(), read()]) as usize;
        if !(1..OPEN_BIT).contains(&id)
            || !((op == 1 && len == 0) || (op == 2 && (1..=4096).contains(&len)))
        {
            for _ in 0..len {
                read();
            }
            return;
        }
        self.frames.push_back(Frame {
            id,
            data: (0..len).map(|_| read()).collect(),
        });
    }
}

pub fn next() -> u32 {
    let mux = unsafe { &mut *SLOT.0.get() };
    while mux.frames.is_empty() {
        mux.receive(read());
    }
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
        mux.receive(crate::uart_poll()?);
    }
}

pub fn close() {
    let mux = unsafe { &mut *SLOT.0.get() };
    mux.frames.retain(|frame| frame.id != mux.active);
    control(3, mux.active);
}
