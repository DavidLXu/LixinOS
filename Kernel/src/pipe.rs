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
        let channel = Rc::as_ptr(&self.0) as usize;
        loop {
            if let Some(n) = self.try_read(data) {
                return n;
            }
            if crate::scheduler::stopped() != 0 {
                return 0;
            }
            crate::scheduler::park(channel);
        }
    }
    pub fn try_read(&self, data: &mut [u8]) -> Option<usize> {
        if data.is_empty() { return Some(0); }
        let mut buf = self.0.borrow_mut();
        let n = data.len().min(buf.bytes.len());
        if n == 0 && buf.writer { return None; }
        let full = buf.bytes.len() == 4096;
        for (dst, b) in data.iter_mut().zip(buf.bytes.drain(..n)) {
            *dst = b;
        }
        if full { crate::scheduler::wake(Rc::as_ptr(&self.0) as usize); }
        Some(n)
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
            if crate::scheduler::stopped() != 0 { return None; }
            match self.try_write(data) {
                Err(()) => return None,
                Ok(Some(n)) => return Some(n),
                Ok(None) => crate::scheduler::park(channel),
            }
        }
    }
    pub fn try_write(&self, data: &[u8]) -> Result<Option<usize>, ()> {
        if data.is_empty() { return Ok(Some(0)); }
        let mut buf = self.0.borrow_mut();
        if !buf.reader { return Err(()); }
        let n = data.len().min(4096 - buf.bytes.len());
        if n == 0 { return Ok(None); }
        let empty = buf.bytes.is_empty();
        buf.bytes.extend(&data[..n]);
        if empty { crate::scheduler::wake(Rc::as_ptr(&self.0) as usize); }
        Ok(Some(n))
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
