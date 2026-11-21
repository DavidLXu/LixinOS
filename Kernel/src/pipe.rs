//! A bounded, byte-oriented pipe. Last endpoint close wakes blocked peers.
use alloc::{collections::VecDeque, rc::Rc};
use core::cell::RefCell;
struct Buffer {
    bytes: VecDeque<u8>,
    reader: bool,
    writer: bool,
}
pub(crate) struct Reader(Rc<RefCell<Buffer>>);
pub(crate) struct Writer(Rc<RefCell<Buffer>>);
pub(crate) fn pair() -> (Rc<Reader>, Rc<Writer>) {
    let shared = Rc::new(RefCell::new(Buffer {
        bytes: VecDeque::with_capacity(4096),
        reader: true,
        writer: true,
    }));
    (Rc::new(Reader(shared.clone())), Rc::new(Writer(shared)))
}
impl Reader {
    pub fn ready(&self) -> bool {
        let b = self.0.borrow();
        !b.bytes.is_empty() || !b.writer
    }
    pub fn read(&self) -> Option<u8> {
        let mut data = [0];
        (self.read_into(&mut data) > 0).then_some(data[0])
    }
    pub fn read_into(&self, data: &mut [u8]) -> usize {
        if data.is_empty() {
            return 0;
        }
        let channel = Rc::as_ptr(&self.0) as usize;
        loop {
            let mut buf = self.0.borrow_mut();
            let n = data.len().min(buf.bytes.len());
            if n != 0 {
                let full = buf.bytes.len() == 4096;
                for (dst, b) in data.iter_mut().zip(buf.bytes.drain(..n)) {
                    *dst = b;
                }
                if full {
                    crate::scheduler::wake(channel);
                }
                return n;
            }
            if !buf.writer || crate::scheduler::stopped() != 0 {
                return 0;
            }
            drop(buf);
            crate::scheduler::park(channel);
        }
    }
}
impl Writer {
    pub fn ready(&self) -> bool {
        let b = self.0.borrow();
        b.bytes.len() < 4096 || !b.reader
    }
    pub fn write(&self, byte: u8) -> bool {
        self.write_from(&[byte]).is_some()
    }
    pub fn write_from(&self, data: &[u8]) -> Option<usize> {
        if data.is_empty() {
            return Some(0);
        }
        let channel = Rc::as_ptr(&self.0) as usize;
        loop {
            let mut buf = self.0.borrow_mut();
            if !buf.reader || crate::scheduler::stopped() != 0 {
                return None;
            }
            let n = data.len().min(4096 - buf.bytes.len());
            if n != 0 {
                let empty = buf.bytes.is_empty();
                buf.bytes.extend(&data[..n]);
                if empty {
                    crate::scheduler::wake(channel);
                }
                return Some(n);
            }
            drop(buf);
            crate::scheduler::park(channel);
        }
    }
}
impl Drop for Reader {
    fn drop(&mut self) {
        self.0.borrow_mut().reader = false;
        crate::scheduler::wake(Rc::as_ptr(&self.0) as usize);
    }
}
impl Drop for Writer {
    fn drop(&mut self) {
        self.0.borrow_mut().writer = false;
        crate::scheduler::wake(Rc::as_ptr(&self.0) as usize);
    }
}
