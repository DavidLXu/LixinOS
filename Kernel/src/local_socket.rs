//! Unix stream endpoints and their in-memory address namespace.
use alloc::{collections::{BTreeMap, VecDeque}, rc::{Rc, Weak}, vec::Vec};
use core::cell::{RefCell, UnsafeCell};
use crate::pipe::{self, Reader, Writer};
use crate::shared_memory::{Shared, RIGHTS, MARKERS};

pub(crate) type Rights = [Option<Shared>; RIGHTS];

struct Marker {
    at: u64,
    rights: Rights,
}

#[derive(Default)]
struct Ancillary {
    read: u64,
    written: u64,
    markers: VecDeque<Marker>,
}

type ReadEnd = (Rc<Reader>, Rc<RefCell<Ancillary>>);
type WriteEnd = (Rc<Writer>, Rc<RefCell<Ancillary>>);

pub(crate) struct Endpoint {
    input: Option<ReadEnd>,
    output: Option<WriteEnd>,
    pub peer: usize,
    pub credentials: [u32;2],
}

impl Endpoint {
    pub fn pair(left: usize, right: usize) -> (Self, Self) {
        let (left_read, right_write) = pipe::pair();
        let (right_read, left_write) = pipe::pair();
        let left_data = Rc::new(RefCell::new(Ancillary::default()));
        let right_data = Rc::new(RefCell::new(Ancillary::default()));
        (
            Self { input: Some((left_read, left_data.clone())), output: Some((left_write, right_data.clone())), peer: right, credentials: crate::permissions::effective_ids() },
            Self { input: Some((right_read, right_data)), output: Some((right_write, left_data)), peer: left, credentials: crate::permissions::effective_ids() },
        )
    }

    pub fn ready(&self) -> u8 {
        u8::from(self.input.as_ref().is_none_or(|(r, _)| r.ready()))
            | (u8::from(self.output.as_ref().is_none_or(|(w, meta)| {
                w.ready() && meta.borrow().markers.len() < MARKERS
            })) << 1)
    }

    pub fn read(&self, data: &mut [u8]) -> (i64, Rights) {
        let empty = core::array::from_fn(|_| None);
        let Some((reader, meta)) = &self.input else { return (0, empty); };
        let mut meta = meta.borrow_mut();
        // Stop before a second control record so each recvmsg reports records in byte order.
        let cap = meta.markers.get(1).map_or(data.len(), |next| {
            data.len().min((next.at - meta.read) as usize)
        });
        let Some(n) = reader.try_read(&mut data[..cap]) else { return (-11, empty); };
        meta.read += n as u64;
        let rights = if meta.markers.front().is_some_and(|first| first.at < meta.read) {
            meta.markers.pop_front().unwrap().rights
        } else { empty };
        (n as i64, rights)
    }

    pub fn write(&self, data: &[u8], rights: Rights) -> i64 {
        let attach = rights[0].is_some();
        if data.is_empty() { return if attach { -22 } else { 0 }; }
        let Some((writer, meta)) = &self.output else { return -32; };
        let mut meta = meta.borrow_mut();
        if attach && meta.markers.len() == MARKERS { return -11; }
        match writer.try_write(data) {
            Ok(Some(n)) => {
                if attach {
                    let at = meta.written;
                    meta.markers.push_back(Marker { at, rights });
                }
                meta.written += n as u64;
                n as i64
            }
            Ok(None) => -11,
            Err(()) => -32,
        }
    }

    pub fn shutdown(&mut self, how: u32) {
        if how != 1 {
            if let Some((_, meta)) = self.input.take() { meta.borrow_mut().markers.clear(); }
        }
        if how != 0 { self.output = None; }
    }
}

impl Drop for Endpoint {
    fn drop(&mut self) { self.shutdown(2); }
}

pub(crate) struct Listener {
    pub pid: usize,
    credentials: [u32;2],
    pub backlog: usize,
    pub pending: VecDeque<Endpoint>,
}

// Kernel code runs serially on one CPU. No registry access can block or yield.
type Registry = BTreeMap<Vec<u8>, Weak<RefCell<Listener>>>;
struct Names(UnsafeCell<Registry>);
unsafe impl Sync for Names {}
static NAMES: Names = Names(UnsafeCell::new(BTreeMap::new()));
const LIMIT: usize = 16;

pub(crate) fn bind(name: &[u8], pid: usize) -> Result<Rc<RefCell<Listener>>, i64> {
    let names = unsafe { &mut *NAMES.0.get() };
    names.retain(|_, slot| slot.strong_count() != 0);
    if names.contains_key(name) { return Err(-98); }
    if names.len() == LIMIT { return Err(-105); }
    let listener = Rc::new(RefCell::new(Listener { pid, credentials:crate::permissions::effective_ids(), backlog: 0, pending: VecDeque::new() }));
    names.insert(name.to_vec(), Rc::downgrade(&listener));
    Ok(listener)
}

pub(crate) fn connect(name: &[u8], pid: usize) -> Result<Endpoint, i64> {
    let names = unsafe { &*NAMES.0.get() };
    let listener = names.get(name).and_then(Weak::upgrade).ok_or(-111i64)?;
    let mut listener = listener.borrow_mut();
    if listener.backlog == 0 { return Err(-111); }
    if listener.pending.len() >= listener.backlog { return Err(-11); }
    let (mut client, server) = Endpoint::pair(pid, listener.pid);
    client.credentials=listener.credentials;
    listener.pending.push_back(server);
    Ok(client)
}
