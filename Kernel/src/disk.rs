//! Two alternating snapshots on a dedicated virtio-blk disk. A flushed header
//! publishes its payload only after all payload sectors have reached the disk.
use crate::agent::Versions;
use alloc::{
    alloc::{Layout, alloc_zeroed, dealloc, handle_alloc_error},
    collections::{BTreeMap, VecDeque},
    rc::Rc,
    string::String,
    vec,
    vec::Vec,
};
use core::{
    cell::{Cell, OnceCell, RefCell},
    ops::Deref,
    ptr::NonNull,
};
use virtio_drivers::{
    BufferDirection, Hal, PhysAddr, device::blk::VirtIOBlk, transport::mmio::MmioTransport,
};

pub type Files = BTreeMap<String, Option<FileData>>;
pub(crate) type Times = BTreeMap<String, FileTime>;
#[derive(Clone, Copy, Default)]
pub(crate) struct FileTime {
    pub birth: u32,
    pub modified: u32,
}

type Result<T> = core::result::Result<T, &'static str>;
const LARGE_SLOT: usize = 192 * 1024 * 1024 / 512;
const BUILD_SLOT: usize = (4usize << 30) / 512;
const LARGE_BODY: usize = 64 * 1024;
const IO_CHUNK: usize = 1024 * 1024;
const SNAPSHOT_BASE: usize = 1; // Sector zero identifies the disk layout.
// Leave room for directories and system files alongside one million user files.
pub const ENTRY_LIMIT: usize = 1_048_576;
pub const FILE_LIMIT: usize = 512 * 1024 * 1024;
const SYSTEM_FILE_LIMIT: usize = 100 * 1024 * 1024;
const MAX_VERSIONS: usize = 1024 * 1024;
const MAGIC: &[u8; 8] = b"LIXINFS1";
const MAGIC_WITH_TIMES: &[u8; 8] = b"LIXINFS3";
const MAGIC_WITH_VERSIONS: &[u8; 8] = b"LIXINFS2";
const INDEX_MAGIC: &[u8; 8] = b"LXIDX001";
const INDEX_FLAG_HASHES: u32 = 1;
// History follows both snapshot slots, including one spare record so a
// torn append cannot overwrite any of the 64 commands still in history.
pub const HISTORY_LIMIT: usize = 64;
const HISTORY_RECORD: usize = 1024;
const HISTORY_SLOTS: usize = HISTORY_LIMIT + 1;
const HISTORY_START: usize = SNAPSHOT_BASE + 2 * LARGE_SLOT;
const DISK_SECTORS: usize = HISTORY_START + HISTORY_SLOTS * (HISTORY_RECORD / 512);
const HISTORY_MAGIC: &[u8; 8] = b"LIXHIST1";

pub(crate) struct Dma;
// Guest RAM is identity mapped, contiguous, and coherent with QEMU's device.
unsafe impl Hal for Dma {
    fn dma_alloc(pages: usize, _: BufferDirection) -> (PhysAddr, NonNull<u8>) {
        let layout = Layout::from_size_align(pages * 4096, 4096).unwrap();
        let p = NonNull::new(unsafe { alloc_zeroed(layout) })
            .unwrap_or_else(|| handle_alloc_error(layout));
        (p.as_ptr() as u64, p)
    }
    unsafe fn dma_dealloc(_: PhysAddr, p: NonNull<u8>, pages: usize) -> i32 {
        unsafe {
            dealloc(
                p.as_ptr(),
                Layout::from_size_align(pages * 4096, 4096).unwrap(),
            )
        };
        0
    }
    unsafe fn mmio_phys_to_virt(p: PhysAddr, _: usize) -> NonNull<u8> {
        NonNull::new(p as *mut u8).unwrap()
    }
    unsafe fn share(p: NonNull<[u8]>, _: BufferDirection) -> PhysAddr {
        p.as_ptr() as *mut u8 as u64
    }
    unsafe fn unshare(_: PhysAddr, _: NonNull<[u8]>, _: BufferDirection) {}
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) struct DiskLoc {
    pub slot: u8,
    pub offset: u32,
    pub len: u32,
    pub checksum: u32,
    pub generation: u64,
}

pub(crate) struct BlockStore {
    dev: VirtIOBlk<Dma, MmioTransport<'static>>,
    slot_sectors: usize,
    generation: u64,
}

impl BlockStore {
    fn read_range(&mut self, slot: u8, payload_offset: usize, mut dst: &mut [u8]) -> Result<()> {
        if dst.is_empty() {
            return Ok(());
        }
        let base = SNAPSHOT_BASE + (slot as usize) * self.slot_sectors + 1;
        let mut sec = payload_offset / 512;
        let head = payload_offset % 512;
        if head != 0 {
            let mut sector = [0u8; 512];
            self.dev
                .read_blocks(base + sec, &mut sector)
                .map_err(|_| "disk read failed")?;
            let take = dst.len().min(512 - head);
            dst[..take].copy_from_slice(&sector[head..head + take]);
            dst = &mut dst[take..];
            sec += 1;
        }
        let full = dst.len() / 512 * 512;
        if full != 0 {
            let (aligned, rest) = dst.split_at_mut(full);
            for chunk in aligned.chunks_mut(IO_CHUNK) {
                #[cfg(feature = "retina")]
                crate::graphics::boot_tick();
                self.dev
                    .read_blocks(base + sec, chunk)
                    .map_err(|_| "disk read failed")?;
                sec += chunk.len() / 512;
            }
            dst = rest;
        }
        if !dst.is_empty() {
            let mut sector = [0u8; 512];
            self.dev
                .read_blocks(base + sec, &mut sector)
                .map_err(|_| "disk read failed")?;
            dst.copy_from_slice(&sector[..dst.len()]);
        }
        Ok(())
    }

    fn read_body(&mut self, loc: DiskLoc) -> Result<Vec<u8>> {
        if loc.generation != self.generation {
            return Err("stale disk file reference");
        }
        let len = loc.len as usize;
        let mut buf = Vec::new();
        buf.try_reserve_exact(len).map_err(|_| "not enough memory")?;
        // SAFETY: u8 is Pod and read_range initializes every byte in 0..len before returning Ok.
        unsafe { buf.set_len(len) };
        self.read_range(loc.slot, loc.offset as usize, &mut buf)?;
        if checksum(&buf) != loc.checksum {
            return Err("corrupted file body");
        }
        Ok(buf)
    }
}

/// File contents backed either in RAM or lazily on the active disk snapshot.
pub struct FileData {
    store: Option<Rc<RefCell<BlockStore>>>,
    loc: Cell<Option<DiskLoc>>,
    data: OnceCell<Rc<Vec<u8>>>,
}

impl FileData {
    pub fn from_vec(bytes: Vec<u8>) -> Self {
        Self {
            store: None,
            loc: Cell::new(None),
            data: OnceCell::from(Rc::new(bytes)),
        }
    }

    pub(crate) fn from_disk(store: Rc<RefCell<BlockStore>>, loc: DiskLoc) -> Self {
        Self {
            store: Some(store),
            loc: Cell::new(Some(loc)),
            data: OnceCell::new(),
        }
    }

    pub fn len(&self) -> usize {
        if let Some(v) = self.data.get() {
            v.len()
        } else {
            self.loc.get().map_or(0, |l| l.len as usize)
        }
    }

    pub fn sig(&self) -> (usize, u32) {
        if let Some(loc) = self.loc.get() {
            (loc.len as usize, loc.checksum)
        } else {
            let v = self.data.get().unwrap();
            (v.len(), checksum(v))
        }
    }

    pub fn try_as_rc(&self) -> Result<Rc<Vec<u8>>> {
        if let Some(v) = self.data.get() {
            return Ok(v.clone());
        }
        let loc = self.loc.get().ok_or("missing file backing")?;
        let store = self.store.as_ref().ok_or("missing disk backing")?;
        let bytes = store.borrow_mut().read_body(loc)?;
        let _ = self.data.set(Rc::new(bytes));
        Ok(self.data.get().unwrap().clone())
    }

    pub fn try_as_slice(&self) -> Result<&[u8]> {
        if let Some(v) = self.data.get() {
            return Ok(v.as_slice());
        }
        let loc = self.loc.get().ok_or("missing file backing")?;
        let store = self.store.as_ref().ok_or("missing disk backing")?;
        let bytes = store.borrow_mut().read_body(loc)?;
        let _ = self.data.set(Rc::new(bytes));
        Ok(self.data.get().unwrap().as_slice())
    }

    pub fn as_slice(&self) -> &[u8] {
        self.try_as_slice().expect("lazy disk file read failed")
    }

    pub fn to_vec(&self) -> Vec<u8> {
        self.as_slice().to_vec()
    }

    pub(crate) fn as_mut_vec(&mut self) -> Result<&mut Vec<u8>> {
        let _ = self.try_as_slice()?;
        self.loc.set(None);
        Ok(Rc::make_mut(self.data.get_mut().unwrap()))
    }

    pub(crate) fn loaded(&self) -> Option<&[u8]> {
        self.data.get().map(|v| v.as_slice())
    }

    pub(crate) fn disk_loc(&self) -> Option<DiskLoc> {
        self.loc.get()
    }

    pub(crate) fn set_disk_loc(&self, loc: DiskLoc) {
        self.loc.set(Some(loc));
    }
}

impl Deref for FileData {
    type Target = [u8];
    fn deref(&self) -> &[u8] {
        self.as_slice()
    }
}

impl Clone for FileData {
    fn clone(&self) -> Self {
        if let Some(v) = self.data.get() {
            Self {
                store: self.store.clone(),
                loc: Cell::new(self.loc.get()),
                data: OnceCell::from(v.clone()),
            }
        } else {
            Self {
                store: self.store.clone(),
                loc: Cell::new(self.loc.get()),
                data: OnceCell::new(),
            }
        }
    }
}

impl PartialEq for FileData {
    fn eq(&self, other: &Self) -> bool {
        if self.len() != other.len() {
            return false;
        }
        if let (Some(a), Some(b)) = (self.loc.get(), other.loc.get()) {
            if a.checksum != b.checksum {
                return false;
            }
            if a == b {
                return true;
            }
        }
        self.as_slice() == other.as_slice()
    }
}
impl Eq for FileData {}

impl PartialEq<[u8]> for FileData {
    fn eq(&self, other: &[u8]) -> bool {
        self.len() == other.len() && self.as_slice() == other
    }
}

impl PartialEq<Vec<u8>> for FileData {
    fn eq(&self, other: &Vec<u8>) -> bool {
        self.len() == other.len() && self.as_slice() == other.as_slice()
    }
}

impl From<Vec<u8>> for FileData {
    fn from(bytes: Vec<u8>) -> Self {
        Self::from_vec(bytes)
    }
}

impl From<&[u8]> for FileData {
    fn from(bytes: &[u8]) -> Self {
        Self::from_vec(bytes.to_vec())
    }
}

struct SeqReader<'a> {
    dev: &'a mut VirtIOBlk<Dma, MmioTransport<'static>>,
    base_sector: usize,
    payload_len: usize,
    buf: Vec<u8>,
    buf_start: usize,
    buf_len: usize,
}

impl<'a> SeqReader<'a> {
    fn new(
        dev: &'a mut VirtIOBlk<Dma, MmioTransport<'static>>,
        base_sector: usize,
        payload_len: usize,
    ) -> Result<Self> {
        let mut buf = Vec::new();
        buf.try_reserve_exact(IO_CHUNK)
            .map_err(|_| "not enough read buffer memory")?;
        Ok(Self {
            dev,
            base_sector,
            payload_len,
            buf,
            buf_start: 0,
            buf_len: 0,
        })
    }

    fn refill(&mut self, offset: usize) -> Result<()> {
        #[cfg(feature = "retina")]
        crate::graphics::boot_tick();
        let aligned = offset / 512 * 512;
        let avail = self.payload_len.saturating_sub(aligned);
        let read_bytes = avail.next_multiple_of(512).min(IO_CHUNK);
        if read_bytes == 0 {
            return Err("unexpected end of snapshot");
        }
        // SAFETY: capacity is IO_CHUNK, read_bytes <= IO_CHUNK, read_blocks initializes all bytes.
        unsafe { self.buf.set_len(read_bytes) };
        self.dev
            .read_blocks(self.base_sector + aligned / 512, &mut self.buf)
            .map_err(|_| "disk read failed")?;
        self.buf_start = aligned;
        self.buf_len = read_bytes.min(avail);
        Ok(())
    }

    fn read_at(&mut self, mut offset: usize, mut dst: &mut [u8]) -> Result<()> {
        if offset.checked_add(dst.len()).is_none_or(|end| end > self.payload_len) {
            return Err("invalid filesystem record");
        }
        while !dst.is_empty() {
            if offset < self.buf_start || offset >= self.buf_start + self.buf_len {
                self.refill(offset)?;
            }
            let pos = offset - self.buf_start;
            let n = dst.len().min(self.buf_len - pos);
            dst[..n].copy_from_slice(&self.buf[pos..pos + n]);
            dst = &mut dst[n..];
            offset += n;
        }
        Ok(())
    }

    fn hash_range(&mut self, mut offset: usize, mut len: usize, mut running: u32) -> Result<(u32, u32)> {
        if offset.checked_add(len).is_none_or(|end| end > self.payload_len) {
            return Err("invalid filesystem record");
        }
        let mut body_hash = checksum(&[]);
        while len != 0 {
            if offset < self.buf_start || offset >= self.buf_start + self.buf_len {
                self.refill(offset)?;
            }
            let pos = offset - self.buf_start;
            let n = len.min(self.buf_len - pos);
            let slice = &self.buf[pos..pos + n];
            running = hash_bytes(running, slice);
            body_hash = hash_bytes(body_hash, slice);
            offset += n;
            len -= n;
        }
        Ok((running, body_hash))
    }
}

#[derive(Clone, Copy)]
struct IndexMeta {
    idx_sec_off: u32,
    idx_bytes: u32,
    idx_checksum: u32,
    versions_off: u32,
    versions_len: u32,
}

fn parse_idx_header(h: &[u8; 512], payload_len: usize, slot_sectors: usize) -> Option<IndexMeta> {
    if &h[28..36] != INDEX_MAGIC {
        return None;
    }
    let ext_sum = u32::from_le_bytes(h[56..60].try_into().unwrap());
    if checksum(&h[..56]) != ext_sum {
        return None;
    }
    let idx_sec_off = u32::from_le_bytes(h[36..40].try_into().unwrap());
    let idx_bytes = u32::from_le_bytes(h[40..44].try_into().unwrap());
    let idx_checksum = u32::from_le_bytes(h[44..48].try_into().unwrap());
    let versions_off = u32::from_le_bytes(h[48..52].try_into().unwrap());
    let versions_len = u32::from_le_bytes(h[52..56].try_into().unwrap());
    let payload_secs = payload_len.div_ceil(512);
    let idx_secs = (idx_bytes as usize).div_ceil(512);
    if (idx_sec_off as usize) < payload_secs
        || idx_bytes < 8
        || 1usize
            .checked_add(idx_sec_off as usize)
            .and_then(|s| s.checked_add(idx_secs))
            .is_none_or(|end| end > slot_sectors)
        || (versions_off as usize)
            .checked_add(versions_len as usize)
            .is_none_or(|end| end > payload_len)
        || versions_len as usize > MAX_VERSIONS
    {
        return None;
    }
    Some(IndexMeta {
        idx_sec_off,
        idx_bytes,
        idx_checksum,
        versions_off,
        versions_len,
    })
}

fn encode_idx_header(h: &mut [u8; 512], meta: IndexMeta) {
    h[28..36].copy_from_slice(INDEX_MAGIC);
    h[36..40].copy_from_slice(&meta.idx_sec_off.to_le_bytes());
    h[40..44].copy_from_slice(&meta.idx_bytes.to_le_bytes());
    h[44..48].copy_from_slice(&meta.idx_checksum.to_le_bytes());
    h[48..52].copy_from_slice(&meta.versions_off.to_le_bytes());
    h[52..56].copy_from_slice(&meta.versions_len.to_le_bytes());
    let sum = checksum(&h[..56]);
    h[56..60].copy_from_slice(&sum.to_le_bytes());
}

struct IndexBuilder {
    buf: Vec<u8>,
    count: u32,
}

impl IndexBuilder {
    fn new(entry_count: usize) -> Result<Self> {
        let mut buf = Vec::new();
        buf.try_reserve(8 + entry_count.saturating_mul(64))
            .map_err(|_| "not enough index memory")?;
        buf.extend_from_slice(&(entry_count as u32).to_le_bytes());
        buf.extend_from_slice(&INDEX_FLAG_HASHES.to_le_bytes());
        Ok(Self { buf, count: 0 })
    }

    #[allow(clippy::too_many_arguments)]
    fn push(
        &mut self,
        path: &str,
        size: u32,
        time: FileTime,
        body_offset: u32,
        body_checksum: u32,
        hash_before: u32,
        hash_after: u32,
    ) -> Result<()> {
        let need = 32 + path.len();
        self.buf
            .try_reserve(need)
            .map_err(|_| "not enough index memory")?;
        self.buf
            .extend_from_slice(&(path.len() as u16).to_le_bytes());
        self.buf.extend_from_slice(&0u16.to_le_bytes());
        self.buf.extend_from_slice(&size.to_le_bytes());
        self.buf.extend_from_slice(&time.birth.to_le_bytes());
        self.buf.extend_from_slice(&time.modified.to_le_bytes());
        self.buf.extend_from_slice(&body_offset.to_le_bytes());
        self.buf.extend_from_slice(&body_checksum.to_le_bytes());
        self.buf.extend_from_slice(&hash_before.to_le_bytes());
        self.buf.extend_from_slice(&hash_after.to_le_bytes());
        self.buf.extend_from_slice(path.as_bytes());
        self.count += 1;
        Ok(())
    }

    fn finish(mut self) -> Vec<u8> {
        self.buf[..4].copy_from_slice(&self.count.to_le_bytes());
        self.buf
    }
}

pub struct Disk {
    store: Rc<RefCell<BlockStore>>,
    slot_sectors: usize,
    pub(crate) scratch_dirty: bool,
    live_bytes: usize,
    revision: u64,
    scratch_records: BTreeMap<String, usize>,
    bodies: BTreeMap<String, CachedBody>,
    generation: u64,
    snapshot_bytes: usize,
    history_sequence: Option<u64>,
    pub(crate) times: Times,
    slot_has_idx: [bool; 2],
    slot_headers: [[u8; 512]; 2],
    pending_other_idx: Option<(usize, IndexMeta)>,
}

pub fn file_counts(files: &Files) -> (usize, usize, usize) {
    let file_count = files.values().flatten().count();
    let content_bytes = files.values().flatten().map(FileData::len).sum();
    (file_count, files.len() - file_count, content_bytes)
}

pub fn resolve_path(cwd: &str, name: &str, is_dir: impl Fn(&str) -> bool) -> Result<String> {
    if name.is_empty() {
        return Err("empty path");
    }
    let mut path = if name.starts_with('/') {
        String::new()
    } else {
        cwd.into()
    };
    for part in name.split('/').filter(|p| !p.is_empty()) {
        if !path.is_empty() && !is_dir(&path) {
            return Err("not a directory");
        }
        match part {
            "." => (),
            ".." => path.truncate(path.rfind('/').unwrap_or(0)),
            _ => {
                if path != "/" {
                    path.push('/');
                }
                path.push_str(part);
            }
        }
    }
    if path.is_empty() {
        path.push('/');
    }
    if path.len() > 512 {
        return Err("path too long");
    }
    Ok(path)
}

pub fn children<'a>(
    files: &'a Files,
    path: &str,
) -> impl Iterator<Item = (&'a str, &'a Option<FileData>)> {
    children_matching(files, path, "", None)
}

pub fn children_matching<'a>(
    files: &'a Files,
    path: &str,
    prefix: &str,
    after: Option<&str>,
) -> impl Iterator<Item = (&'a str, &'a Option<FileData>)> {
    use core::ops::Bound::{Excluded, Included, Unbounded};
    let base = alloc::format!("{}/", path.trim_end_matches('/'));
    let prefix_key = alloc::format!("{base}{prefix}");
    let start = after.filter(|name| *name >= prefix).map_or_else(
        || Included(prefix_key.clone()),
        |name| Excluded(alloc::format!("{base}{name}")),
    );
    files
        .range((start, Unbounded))
        .take_while(move |(path, _)| path.starts_with(&prefix_key))
        .map(move |(path, value)| (&path[base.len()..], value))
        .filter(|(name, _)| !name.is_empty() && !name.contains('/'))
}

#[inline]
fn hash_bytes(mut hash: u32, bytes: &[u8]) -> u32 {
    let mut chunks = bytes.chunks_exact(8);
    for chunk in &mut chunks {
        let v = u64::from_le_bytes(chunk.try_into().unwrap());
        hash = (hash ^ (v as u8 as u32)).wrapping_mul(16777619);
        hash = (hash ^ ((v >> 8) as u8 as u32)).wrapping_mul(16777619);
        hash = (hash ^ ((v >> 16) as u8 as u32)).wrapping_mul(16777619);
        hash = (hash ^ ((v >> 24) as u8 as u32)).wrapping_mul(16777619);
        hash = (hash ^ ((v >> 32) as u8 as u32)).wrapping_mul(16777619);
        hash = (hash ^ ((v >> 40) as u8 as u32)).wrapping_mul(16777619);
        hash = (hash ^ ((v >> 48) as u8 as u32)).wrapping_mul(16777619);
        hash = (hash ^ ((v >> 56) as u8 as u32)).wrapping_mul(16777619);
    }
    for &byte in chunks.remainder() {
        hash = (hash ^ u32::from(byte)).wrapping_mul(16777619);
    }
    hash
}
pub(crate) fn checksum(bytes: &[u8]) -> u32 {
    hash_bytes(2166136261, bytes)
}

// Only metadata is cached. An offset certifies that this exact, unmodified file
// body occupies that position in the corresponding physical snapshot slot.
#[derive(Default)]
struct CachedBody {
    offsets: [Option<usize>; 2],
    hash: Option<(u32, u32)>,
}
impl CachedBody {
    fn hash_slice(&mut self, initial: u32, bytes: &[u8]) -> u32 {
        if let Some((start, end)) = self.hash {
            if initial == start {
                return end;
            }
        }
        let end = hash_bytes(initial, bytes);
        self.hash = Some((initial, end));
        end
    }
}

// All directories come first in lexical (parent-first) order. Large bodies
// precede small files so editing a source line keeps the large FNV prefix valid.
fn snapshot_entries(files: &Files) -> impl Iterator<Item = (&String, &Option<FileData>)> {
    // The launcher reads this hint with a bounded scan on million-entry disks.
    let hint = "/.desktop-boot";
    files
        .get_key_value("/")
        .into_iter()
        .chain(files.get_key_value(hint))
        .chain(
            files
                .iter()
                .filter(move |(k, v)| v.is_none() && k.as_str() != "/" && k.as_str() != hint),
        )
        .chain(files.iter().filter(move |(k, v)| {
            k.as_str() != hint && v.as_ref().is_some_and(|b| b.len() >= LARGE_BODY)
        }))
        .chain(files.iter().filter(move |(k, v)| {
            k.as_str() != hint && v.as_ref().is_some_and(|b| b.len() < LARGE_BODY)
        }))
}

struct SnapshotWriter<'a> {
    device: &'a mut VirtIOBlk<Dma, MmioTransport<'static>>,
    sector: usize,
    origin: usize,
    chunk: Vec<u8>,
    hash: u32,
}

impl SnapshotWriter<'_> {
    fn offset(&self) -> usize {
        (self.sector - self.origin) * 512 + self.chunk.len()
    }

    fn push(&mut self, bytes: &[u8]) -> Result<()> {
        self.hash = hash_bytes(self.hash, bytes);
        self.append(bytes)
    }

    fn body_loaded(
        &mut self,
        bytes: &[u8],
        cached: &mut CachedBody,
        slot: usize,
    ) -> Result<()> {
        let offset = self.offset();
        self.hash = cached.hash_slice(self.hash, bytes);
        if cached.offsets[slot] == Some(offset) {
            // Boundary sectors also contain a neighbouring pathname or timestamp.
            let prefix = (512 - offset % 512) % 512;
            self.append(&bytes[..prefix])?;
            self.flush_chunk()?;
            let sectors = (bytes.len() - prefix) / 512;
            self.sector += sectors;
            self.append(&bytes[prefix + sectors * 512..])?;
        } else {
            self.append(bytes)?;
            cached.offsets[slot] = Some(offset);
        }
        Ok(())
    }

    fn append(&mut self, mut bytes: &[u8]) -> Result<()> {
        while !bytes.is_empty() {
            let count = bytes.len().min(IO_CHUNK - self.chunk.len());
            let (part, remaining) = bytes.split_at(count);
            self.chunk.extend_from_slice(part);
            bytes = remaining;
            if self.chunk.len() == IO_CHUNK {
                self.flush_chunk()?;
            }
        }
        Ok(())
    }

    fn flush_chunk(&mut self) -> Result<()> {
        #[cfg(feature = "retina")]
        crate::graphics::boot_tick();
        if self.chunk.is_empty() {
            return Ok(());
        }
        self.chunk.resize(self.chunk.len().next_multiple_of(512), 0);
        self.device
            .write_blocks(self.sector, &self.chunk)
            .map_err(|_| "disk commit failed")?;
        self.sector += self.chunk.len() / 512;
        self.chunk.clear();
        Ok(())
    }
}

fn take<'a>(src: &mut &'a [u8], n: usize) -> Result<&'a [u8]> {
    let (first, rest) = src.split_at_checked(n).ok_or("invalid filesystem record")?;
    *src = rest;
    Ok(first)
}
fn number(src: &mut &[u8]) -> Result<u32> {
    Ok(u32::from_le_bytes(take(src, 4)?.try_into().unwrap()))
}
pub(crate) fn decode(mut src: &[u8], format: &[u8]) -> Result<(Files, Versions, Times)> {
    let count = number(&mut src)? as usize;
    if count > ENTRY_LIMIT {
        return Err("invalid filesystem size");
    }
    let mut files = Files::new();
    let mut times = Times::new();
    for _ in 0..count {
        let n = number(&mut src)? as usize;
        let size = number(&mut src)?;
        let time = if format == MAGIC_WITH_TIMES {
            FileTime {
                birth: number(&mut src)?,
                modified: number(&mut src)?,
            }
        } else {
            FileTime::default()
        };
        if n > 512 {
            return Err("invalid path length");
        }
        let path = core::str::from_utf8(take(&mut src, n)?).map_err(|_| "invalid path")?;
        if path != "/" {
            let (parent, name) = path.rsplit_once('/').ok_or("invalid path")?;
            if matches!(name, "" | "." | "..")
                || files.get(if parent.is_empty() { "/" } else { parent }) != Some(&None)
            {
                return Err("invalid directory tree");
            }
        }
        let value = if size == u32::MAX {
            None
        } else {
            if size as usize > FILE_LIMIT {
                return Err("invalid file size");
            }
            Some(FileData::from_vec(take(&mut src, size as usize)?.to_vec()))
        };
        if time.birth != 0 || time.modified != 0 {
            times.insert(path.into(), time);
        }
        if files.insert(path.into(), value).is_some() {
            return Err("duplicate path");
        }
    }
    if files.get("/") != Some(&None) {
        return Err("invalid filesystem");
    }
    let versions =
        if format == MAGIC_WITH_VERSIONS || (format == MAGIC_WITH_TIMES && !src.is_empty()) {
            if src.len() > MAX_VERSIONS {
                return Err("invalid agent version data size");
            }
            let versions: Versions =
                serde_json_wasm::from_slice(src).map_err(|_| "invalid agent version data")?;
            if versions.iter().any(|(path, version)| {
                !path.ends_with(".py")
                    || !files.get(path).is_some_and(Option::is_some)
                    || !version.valid()
            }) {
                return Err("invalid agent version record");
            }
            versions
        } else {
            if !src.is_empty() {
                return Err("invalid filesystem");
            }
            Versions::new()
        };
    Ok((files, versions, times))
}

fn read_index_blob(
    dev: &mut VirtIOBlk<Dma, MmioTransport<'static>>,
    slot: usize,
    slot_sectors: usize,
    meta: IndexMeta,
) -> Result<Vec<u8>> {
    let bytes = meta.idx_bytes as usize;
    let rounded = bytes.next_multiple_of(512);
    let mut buf = Vec::new();
    buf.try_reserve_exact(rounded)
        .map_err(|_| "not enough index memory")?;
    // SAFETY: u8 is Pod and read_blocks fills every byte in 0..rounded.
    unsafe { buf.set_len(rounded) };
    let base = SNAPSHOT_BASE + slot * slot_sectors + 1 + meta.idx_sec_off as usize;
    for (i, chunk) in buf.chunks_mut(IO_CHUNK).enumerate() {
        #[cfg(feature = "retina")]
        crate::graphics::boot_tick();
        dev.read_blocks(base + i * (IO_CHUNK / 512), chunk)
            .map_err(|_| "disk read failed")?;
    }
    buf.truncate(bytes);
    if checksum(&buf) != meta.idx_checksum {
        return Err("corrupted snapshot index");
    }
    Ok(buf)
}

fn write_index_blob(
    dev: &mut VirtIOBlk<Dma, MmioTransport<'static>>,
    slot: usize,
    slot_sectors: usize,
    idx_sec_off: u32,
    mut idx: Vec<u8>,
) -> Result<()> {
    let rounded = idx.len().next_multiple_of(512);
    idx.resize(rounded, 0);
    let base = SNAPSHOT_BASE + slot * slot_sectors + 1 + idx_sec_off as usize;
    for (i, chunk) in idx.chunks(IO_CHUNK).enumerate() {
        dev.write_blocks(base + i * (IO_CHUNK / 512), chunk)
            .map_err(|_| "disk commit failed")?;
    }
    Ok(())
}

impl Disk {
    pub(crate) fn capacity_bytes(&self) -> usize {
        (self.slot_sectors - 1) * 512
    }
    pub(crate) fn scratch_path(&self, path: &str) -> bool {
        self.slot_sectors > LARGE_SLOT && (path.starts_with("/scratch/") || path.starts_with("/tmp/"))
    }
    fn history_start(&self) -> usize {
        SNAPSHOT_BASE + 2 * self.slot_sectors
    }
    pub(crate) fn file_limit(&self) -> usize {
        if self.slot_sectors > LARGE_SLOT {
            FILE_LIMIT
        } else {
            SYSTEM_FILE_LIMIT
        }
    }

    pub(crate) fn write_scratch(
        &mut self,
        path: &str,
        file: &mut Vec<u8>,
        start: usize,
        data: &[u8],
    ) -> Result<()> {
        let end = start.checked_add(data.len()).ok_or("file too large")?;
        let additional = end.saturating_sub(file.len());
        if end > self.file_limit() || self.live_bytes + additional > self.capacity_bytes() {
            return Err("filesystem full");
        }
        self.invalidate_body(path);
        let record = self
            .scratch_records
            .get_mut(path)
            .ok_or("missing scratch file")?;
        file.try_reserve(additional)
            .map_err(|_| "not enough memory")?;
        file.resize(file.len() + additional, 0);
        file[start..end].copy_from_slice(data);
        *record += additional;
        self.live_bytes += additional;
        self.times
            .entry(String::from(path))
            .or_default()
            .modified = crate::utility::wall_seconds();
        self.revision = self.revision.wrapping_add(1);
        self.scratch_dirty = true;
        Ok(())
    }

    pub(crate) fn used_bytes(&self) -> usize {
        self.live_bytes
    }
    pub(crate) fn revision(&self) -> u64 {
        self.revision
    }

    fn record_bytes(path: &str, entry: &Option<FileData>) -> usize {
        16 + path.len() + entry.as_ref().map_or(0, FileData::len)
    }

    fn index_scratch(&mut self, files: &Files, len: usize) {
        self.live_bytes = len;
        if self.slot_sectors > LARGE_SLOT {
            self.scratch_records = files
                .iter().filter(|(path,_)|self.scratch_path(path) || path.as_str()==crate::permissions::METADATA)
                .map(|(path, entry)| (path.clone(), Self::record_bytes(path, entry)))
                .collect();
        }
    }

    // Admit the transaction only if the resulting tree can still be checkpointed.
    pub(crate) fn write_back(
        &mut self,
        files: &Files,
        paths: &[(&str, Option<FileTime>)],
    ) -> Result<()> {
        let mut records = BTreeMap::new();
        for &(path, _) in paths {
            if self.scratch_path(path) || path==crate::permissions::METADATA {
                let size = files
                    .get(path)
                    .map_or(0, |entry| Self::record_bytes(path, entry));
                records.insert(path, size);
            }
        }
        let old: usize = records
            .keys()
            .filter_map(|p| self.scratch_records.get(*p))
            .sum();
        let len = self.live_bytes - old + records.values().sum::<usize>();
        if len > self.capacity_bytes() {
            return Err("filesystem full");
        }
        for (path, size) in records {
            if size == 0 {
                self.scratch_records.remove(path);
            } else {
                self.scratch_records.insert(String::from(path), size);
            }
        }
        self.live_bytes = len;
        self.scratch_dirty = true;
        self.revision = self.revision.wrapping_add(1);
        Ok(())
    }

    pub fn snapshot_status(&self) -> (u64, u64) {
        (self.store.borrow().dev.capacity(), self.generation)
    }

    // Native kernels, host tooling and Rust can serialize the same tree in
    // different orders. Walk physical record headers, skipping file bodies.
    pub(crate) fn verify_file(&mut self, files: &Files, path: &str) -> Result<u64> {
        let bytes = files
            .get(path)
            .and_then(Option::as_ref)
            .ok_or("saved file not found")?
            .try_as_slice()?;
        let base = SNAPSHOT_BASE + (self.generation as usize % 2) * self.slot_sectors;
        let mut store = self.store.borrow_mut();
        let mut header = [0u8; 512];
        store
            .dev
            .read_blocks(base, &mut header)
            .map_err(|_| "snapshot read failed")?;
        if header[8..16] != self.generation.to_le_bytes()
            || header[16..20] != (self.snapshot_bytes as u32).to_le_bytes()
            || header[24..28] != checksum(&header[..24]).to_le_bytes()
        {
            return Err("committed snapshot header does not match the mounted disk");
        }
        let record_header = match &header[..8] {
            format if format == MAGIC_WITH_TIMES => 16,
            format if format == MAGIC || format == MAGIC_WITH_VERSIONS => 8,
            _ => return Err("invalid snapshot format"),
        };
        if self.scratch_dirty {
            return Err("build filesystem has pending writes");
        }
        let mut page = [0u8; 4096];
        let mut page_start = usize::MAX;
        let mut read = |mut at: usize, mut dst: &mut [u8]| -> Result<()> {
            if at + dst.len() > self.snapshot_bytes {
                return Err("truncated file record");
            }
            while !dst.is_empty() {
                let page_at = at / page.len() * page.len();
                if page_at != page_start {
                    let n = page
                        .len()
                        .min((self.snapshot_bytes - page_at).next_multiple_of(512));
                    store
                        .dev
                        .read_blocks(base + 1 + page_at / 512, &mut page[..n])
                        .map_err(|_| "snapshot record read failed")?;
                    page_start = page_at;
                }
                let within = at - page_at;
                let n = dst.len().min(page.len() - within);
                dst[..n].copy_from_slice(&page[within..within + n]);
                dst = &mut dst[n..];
                at += n;
            }
            Ok(())
        };
        let mut offset = 4;
        let mut found = false;
        for _ in 0..files.len() {
            let mut fields = [0u8; 8];
            read(offset, &mut fields)?;
            let name_len = u32::from_le_bytes(fields[..4].try_into().unwrap()) as usize;
            let size = u32::from_le_bytes(fields[4..].try_into().unwrap());
            if name_len > 512 {
                return Err("invalid record pathname");
            }
            if name_len == path.len() && size as usize == bytes.len() {
                let mut name = [0u8; 512];
                read(offset + record_header, &mut name[..name_len])?;
                if &name[..name_len] == path.as_bytes() {
                    found = true;
                    break;
                }
            }
            offset += record_header + name_len + if size == u32::MAX { 0 } else { size as usize };
        }
        if !found {
            return Err("file missing from committed snapshot");
        }
        let end = offset + record_header + path.len() + bytes.len();
        if end > self.snapshot_bytes {
            return Err("file record is outside the committed snapshot");
        }
        let start = offset / 512 * 512;
        let mut window = vec![0; end.next_multiple_of(512) - start];
        store
            .dev
            .read_blocks(base + 1 + start / 512, &mut window)
            .map_err(|_| "saved file readback failed")?;
        let record = &window[offset - start..end - start];
        let data = record_header + path.len();
        if record[..4] != (path.len() as u32).to_le_bytes()
            || record[4..8] != (bytes.len() as u32).to_le_bytes()
            || record[record_header..data] != *path.as_bytes()
            || record[data..] != *bytes
        {
            return Err("saved file differs from the committed disk record");
        }
        Ok(self.generation)
    }

    #[cfg(feature = "retina")]
    fn report_storage(&self, files: &Files, payload_bytes: usize) {
        let capacity = self.capacity_bytes();
        crate::graphics::set_storage_usage(
            files.values().flatten().count(),
            payload_bytes,
            capacity,
        );
    }

    pub fn mount() -> Result<(Self, Files, Versions)> {
        // Launcher connects the disk to virtio-mmio-bus.0 on QEMU virt.
        let transport =
            unsafe { MmioTransport::new(NonNull::new(0x0a00_0000 as *mut _).unwrap(), 512) }
                .map_err(|_| "no virtio disk")?;
        let mut dev = VirtIOBlk::new(transport).map_err(|_| "disk initialization failed")?;
        if dev.capacity() < DISK_SECTORS as u64 {
            return Err("disk too small for LIXDISK4");
        }
        let mut layout = [0u8; 512];
        dev.read_blocks(0, &mut layout)
            .map_err(|_| "disk read failed")?;
        let slot_sectors = match &layout[..8] {
            b"LIXDISK4" => LARGE_SLOT,
            b"LIXBLD01" if crate::high_heap_end().is_some() => {
                let sectors = dev.capacity() as usize - 1 - HISTORY_SLOTS * HISTORY_RECORD / 512;
                let slot = sectors / 2;
                if sectors % 2 != 0 || (slot != (3usize << 30) / 512 && slot != BUILD_SLOT) {
                    return Err("unsupported build volume size");
                }
                slot
            }
            b"LIXBLD01" => return Err("build disk requires a large-memory VM"),
            _ => return Err("unsupported disk layout"),
        };
        if dev.capacity() < (1 + 2 * slot_sectors + HISTORY_SLOTS * HISTORY_RECORD / 512) as u64
            || layout[8..].iter().any(|b| *b != 0)
        {
            return Err("unsupported disk layout");
        }
        let store = Rc::new(RefCell::new(BlockStore {
            dev,
            slot_sectors,
            generation: 0,
        }));
        let mut disk = Self {
            store,
            slot_sectors,
            bodies: BTreeMap::new(),
            scratch_dirty: false,
            scratch_records: BTreeMap::new(),
            live_bytes: 0,
            revision: 0,
            generation: 0,
            snapshot_bytes: 0,
            history_sequence: None,
            times: Times::new(),
            slot_has_idx: [false; 2],
            slot_headers: [[0u8; 512]; 2],
            pending_other_idx: None,
        };
        if let Some((files, versions)) = disk.read_pair()? {
            return Ok((disk, files, versions));
        }
        let files = Files::from([("/".into(), None)]);
        let now = crate::utility::wall_seconds();
        disk.times.insert(
            "/".into(),
            FileTime {
                birth: now,
                modified: now,
            },
        );
        let versions = Versions::new();
        disk.save(&files, &versions)?;
        Ok((disk, files, versions))
    }

    fn mount_indexed(
        &mut self,
        slot: usize,
        gen_id: u64,
        len: usize,
        meta: IndexMeta,
    ) -> Result<(Files, Versions, Times, BTreeMap<String, CachedBody>)> {
        let idx = {
            let mut store = self.store.borrow_mut();
            read_index_blob(&mut store.dev, slot, self.slot_sectors, meta)?
        };
        let mut src = idx.as_slice();
        let count = number(&mut src)? as usize;
        let flags = number(&mut src)?;
        if count > ENTRY_LIMIT {
            return Err("invalid filesystem size");
        }
        let mut files = Files::new();
        let mut times = Times::new();
        let mut bodies = BTreeMap::new();
        let has_hashes = (flags & INDEX_FLAG_HASHES) != 0;
        let mut last_parent: &str = "";
        for _ in 0..count {
            let hdr = take(&mut src, 32)?;
            let path_len = u16::from_le_bytes(hdr[0..2].try_into().unwrap()) as usize;
            let size = u32::from_le_bytes(hdr[4..8].try_into().unwrap());
            let birth = u32::from_le_bytes(hdr[8..12].try_into().unwrap());
            let modified = u32::from_le_bytes(hdr[12..16].try_into().unwrap());
            let body_offset = u32::from_le_bytes(hdr[16..20].try_into().unwrap());
            let body_checksum = u32::from_le_bytes(hdr[20..24].try_into().unwrap());
            let hash_before = u32::from_le_bytes(hdr[24..28].try_into().unwrap());
            let hash_after = u32::from_le_bytes(hdr[28..32].try_into().unwrap());
            if path_len == 0 || path_len > 512 {
                return Err("invalid path length");
            }
            let path = core::str::from_utf8(take(&mut src, path_len)?).map_err(|_| "invalid path")?;
            if path != "/" {
                let (parent, name) = path.rsplit_once('/').ok_or("invalid path")?;
                let parent = if parent.is_empty() { "/" } else { parent };
                if matches!(name, "" | "." | "..")
                    || (parent != last_parent && files.get(parent) != Some(&None))
                {
                    return Err("invalid directory tree");
                }
                last_parent = parent;
            }
            let value = if size == u32::MAX {
                None
            } else {
                if size as usize > FILE_LIMIT
                    || (body_offset as usize)
                        .checked_add(size as usize)
                        .is_none_or(|end| end > len)
                {
                    return Err("invalid file size");
                }
                if size as usize >= LARGE_BODY {
                    let mut cached = CachedBody::default();
                    cached.offsets[slot] = Some(body_offset as usize);
                    if has_hashes {
                        cached.hash = Some((hash_before, hash_after));
                    }
                    bodies.insert(String::from(path), cached);
                }
                Some(FileData::from_disk(
                    self.store.clone(),
                    DiskLoc {
                        slot: slot as u8,
                        offset: body_offset,
                        len: size,
                        checksum: body_checksum,
                        generation: gen_id,
                    },
                ))
            };
            if birth != 0 || modified != 0 {
                times.insert(String::from(path), FileTime { birth, modified });
            }
            if files.insert(String::from(path), value).is_some() {
                return Err("duplicate path");
            }
        }
        if !src.is_empty() || files.get("/") != Some(&None) {
            return Err("invalid filesystem");
        }
        let versions = if meta.versions_len != 0 {
            let mut vbuf = vec![0u8; meta.versions_len as usize];
            self.store
                .borrow_mut()
                .read_range(slot as u8, meta.versions_off as usize, &mut vbuf)?;
            let versions: Versions =
                serde_json_wasm::from_slice(&vbuf).map_err(|_| "invalid agent version data")?;
            if versions.iter().any(|(path, version)| {
                !path.ends_with(".py")
                    || !files.get(path).is_some_and(Option::is_some)
                    || !version.valid()
            }) {
                return Err("invalid agent version record");
            }
            versions
        } else {
            Versions::new()
        };
        Ok((files, versions, times, bodies))
    }

    fn mount_streaming(
        &mut self,
        slot: usize,
        gen_id: u64,
        len: usize,
        format: &[u8],
        expected_hash: [u8; 4],
        header: &[u8; 512],
    ) -> Result<(Files, Versions, Times, BTreeMap<String, CachedBody>, bool)> {
        let base_sector = SNAPSHOT_BASE + slot * self.slot_sectors + 1;
        let is_timed = format == MAGIC_WITH_TIMES;
        let mut store = self.store.borrow_mut();
        let mut reader = SeqReader::new(&mut store.dev, base_sector, len)?;
        let mut count_bytes = [0u8; 4];
        reader.read_at(0, &mut count_bytes)?;
        let mut running = hash_bytes(checksum(&[]), &count_bytes);
        let count = u32::from_le_bytes(count_bytes) as usize;
        if count > ENTRY_LIMIT {
            return Err("invalid filesystem size");
        }
        let mut files = Files::new();
        let mut times = Times::new();
        let mut bodies = BTreeMap::new();
        let mut idx_builder = if is_timed {
            IndexBuilder::new(count).ok()
        } else {
            None
        };
        let mut offset = 4usize;
        let mut name_buf = [0u8; 512];
        for _ in 0..count {
            let rec_len = if is_timed { 16 } else { 8 };
            let mut rec = [0u8; 16];
            reader.read_at(offset, &mut rec[..rec_len])?;
            running = hash_bytes(running, &rec[..rec_len]);
            offset += rec_len;
            let n = u32::from_le_bytes(rec[0..4].try_into().unwrap()) as usize;
            let size = u32::from_le_bytes(rec[4..8].try_into().unwrap());
            let time = if is_timed {
                FileTime {
                    birth: u32::from_le_bytes(rec[8..12].try_into().unwrap()),
                    modified: u32::from_le_bytes(rec[12..16].try_into().unwrap()),
                }
            } else {
                FileTime::default()
            };
            if n == 0 || n > 512 {
                return Err("invalid path length");
            }
            reader.read_at(offset, &mut name_buf[..n])?;
            running = hash_bytes(running, &name_buf[..n]);
            offset += n;
            let path = core::str::from_utf8(&name_buf[..n]).map_err(|_| "invalid path")?;
            if path != "/" {
                let (parent, name) = path.rsplit_once('/').ok_or("invalid path")?;
                if matches!(name, "" | "." | "..")
                    || files.get(if parent.is_empty() { "/" } else { parent }) != Some(&None)
                {
                    return Err("invalid directory tree");
                }
            }
            let value = if size == u32::MAX {
                if let Some(builder) = idx_builder.as_mut() {
                    if builder.push(path, u32::MAX, time, 0, 0, 0, 0).is_err() {
                        idx_builder = None;
                    }
                }
                None
            } else {
                if size as usize > FILE_LIMIT {
                    return Err("invalid file size");
                }
                let body_offset = offset;
                let hash_before = running;
                let (next_running, body_sum) = reader.hash_range(body_offset, size as usize, running)?;
                running = next_running;
                offset += size as usize;
                if size as usize >= LARGE_BODY {
                    let mut cached = CachedBody::default();
                    cached.offsets[slot] = Some(body_offset);
                    cached.hash = Some((hash_before, running));
                    bodies.insert(String::from(path), cached);
                }
                if let Some(builder) = idx_builder.as_mut() {
                    if builder
                        .push(
                            path,
                            size,
                            time,
                            body_offset as u32,
                            body_sum,
                            hash_before,
                            running,
                        )
                        .is_err()
                    {
                        idx_builder = None;
                    }
                }
                Some(FileData::from_disk(
                    self.store.clone(),
                    DiskLoc {
                        slot: slot as u8,
                        offset: body_offset as u32,
                        len: size,
                        checksum: body_sum,
                        generation: gen_id,
                    },
                ))
            };
            if time.birth != 0 || time.modified != 0 {
                times.insert(String::from(path), time);
            }
            if files.insert(String::from(path), value).is_some() {
                return Err("duplicate path");
            }
        }
        if files.get("/") != Some(&None) {
            return Err("invalid filesystem");
        }
        let versions_off = offset;
        let versions_len = len - offset;
        let versions = if format == MAGIC_WITH_VERSIONS || (is_timed && versions_len != 0) {
            if versions_len > MAX_VERSIONS {
                return Err("invalid agent version data size");
            }
            let mut vbuf = vec![0u8; versions_len];
            reader.read_at(versions_off, &mut vbuf)?;
            running = hash_bytes(running, &vbuf);
            if running.to_le_bytes() != expected_hash {
                return Err("corrupted snapshot");
            }
            let versions: Versions =
                serde_json_wasm::from_slice(&vbuf).map_err(|_| "invalid agent version data")?;
            if versions.iter().any(|(path, version)| {
                !path.ends_with(".py")
                    || !files.get(path).is_some_and(Option::is_some)
                    || !version.valid()
            }) {
                return Err("invalid agent version record");
            }
            versions
        } else {
            if versions_len != 0 {
                return Err("invalid filesystem");
            }
            if running.to_le_bytes() != expected_hash {
                return Err("corrupted snapshot");
            }
            Versions::new()
        };
        drop(reader);

        // Attach LXIDX001 to this already-verified LIXINFS3 slot so the next boot is instant.
        let mut attached = false;
        if let Some(builder) = idx_builder {
            let idx = builder.finish();
            let idx_sec_off = len.div_ceil(512);
            let idx_secs = idx.len().div_ceil(512);
            if 1 + idx_sec_off + idx_secs <= self.slot_sectors {
                let meta = IndexMeta {
                    idx_sec_off: idx_sec_off as u32,
                    idx_bytes: idx.len() as u32,
                    idx_checksum: checksum(&idx),
                    versions_off: versions_off as u32,
                    versions_len: versions_len as u32,
                };
                if write_index_blob(&mut store.dev, slot, self.slot_sectors, meta.idx_sec_off, idx)
                    .is_ok()
                    && store.dev.flush().is_ok()
                {
                    let mut updated = *header;
                    encode_idx_header(&mut updated, meta);
                    let head_sec = SNAPSHOT_BASE + slot * self.slot_sectors;
                    if store.dev.write_blocks(head_sec, &updated).is_ok()
                        && store.dev.flush().is_ok()
                    {
                        self.slot_headers[slot] = updated;
                        attached = true;
                    }
                }
            }
        }
        Ok((files, versions, times, bodies, attached))
    }

    fn read_pair(&mut self) -> Result<Option<(Files, Versions)>> {
        let mut heads = [[0u8; 512]; 2];
        {
            let mut store = self.store.borrow_mut();
            for (slot, h) in heads.iter_mut().enumerate() {
                store
                    .dev
                    .read_blocks(SNAPSHOT_BASE + slot * self.slot_sectors, h)
                    .map_err(|_| "disk read failed")?;
            }
        }
        self.slot_headers = heads;
        let generation = |h: &[u8; 512]| u64::from_le_bytes(h[8..16].try_into().unwrap());
        let newest = usize::from(generation(&heads[1]) > generation(&heads[0]));
        for slot in [newest, 1 - newest] {
            let h = &heads[slot];
            let format = &h[..8];
            if ![MAGIC.as_slice(), MAGIC_WITH_VERSIONS, MAGIC_WITH_TIMES].contains(&format)
                || checksum(&h[..24]).to_le_bytes() != h[24..28]
            {
                continue;
            }
            let len = u32::from_le_bytes(h[16..20].try_into().unwrap()) as usize;
            if !(4..=self.capacity_bytes()).contains(&len) {
                continue;
            }
            let gen_id = generation(h);
            if format == MAGIC_WITH_TIMES {
                if let Some(meta) = parse_idx_header(h, len, self.slot_sectors) {
                    if let Ok((files, versions, times, bodies)) =
                        self.mount_indexed(slot, gen_id, len, meta)
                    {
                        self.times = times;
                        self.bodies = bodies;
                        self.generation = gen_id;
                        self.store.borrow_mut().generation = gen_id;
                        self.snapshot_bytes = len;
                        self.revision = gen_id;
                        self.slot_has_idx[slot] = true;
                        let other = 1 - slot;
                        let oh = &heads[other];
                        if &oh[..8] == MAGIC_WITH_TIMES
                            && checksum(&oh[..24]).to_le_bytes() == oh[24..28]
                        {
                            let olen = u32::from_le_bytes(oh[16..20].try_into().unwrap()) as usize;
                            if (4..=self.capacity_bytes()).contains(&olen) {
                                if let Some(ometa) = parse_idx_header(oh, olen, self.slot_sectors) {
                                    self.slot_has_idx[other] = true;
                                    if generation(oh).checked_add(1) == Some(gen_id) {
                                        self.pending_other_idx = Some((other, ometa));
                                    }
                                }
                            }
                        }
                        self.index_scratch(&files, len);
                        #[cfg(feature = "retina")]
                        self.report_storage(&files, len);
                        return Ok(Some((files, versions)));
                    }
                }
            }
            let expected: [u8; 4] = h[20..24].try_into().unwrap();
            if let Ok((files, versions, times, bodies, attached)) =
                self.mount_streaming(slot, gen_id, len, format, expected, h)
            {
                self.times = times;
                self.bodies = bodies;
                self.generation = gen_id;
                self.store.borrow_mut().generation = gen_id;
                self.snapshot_bytes = len;
                self.revision = gen_id;
                self.slot_has_idx[slot] = attached;
                self.index_scratch(&files, len);
                #[cfg(feature = "retina")]
                self.report_storage(&files, len);
                return Ok(Some((files, versions)));
            }
        }
        if heads.iter().flatten().any(|b| *b != 0) {
            return Err("no valid filesystem snapshot");
        }
        Ok(None)
    }

    fn load_pending_other_offsets(&mut self, target_slot: usize, files: &Files) {
        let Some((other, meta)) = self.pending_other_idx.take() else {
            return;
        };
        if other != target_slot {
            return;
        }
        let Ok(idx) = (|| {
            let mut store = self.store.borrow_mut();
            read_index_blob(&mut store.dev, other, self.slot_sectors, meta)
        })() else {
            return;
        };
        let mut src = idx.as_slice();
        let Ok(count) = number(&mut src) else { return };
        if number(&mut src).is_err() || count as usize > ENTRY_LIMIT {
            return;
        }
        for _ in 0..count {
            let Ok(hdr) = take(&mut src, 32) else { return };
            let path_len = u16::from_le_bytes(hdr[0..2].try_into().unwrap()) as usize;
            let size = u32::from_le_bytes(hdr[4..8].try_into().unwrap());
            let body_offset = u32::from_le_bytes(hdr[16..20].try_into().unwrap());
            let body_checksum = u32::from_le_bytes(hdr[20..24].try_into().unwrap());
            let Ok(path_bytes) = take(&mut src, path_len) else {
                return;
            };
            if size == u32::MAX || (size as usize) < LARGE_BODY {
                continue;
            }
            let Ok(path) = core::str::from_utf8(path_bytes) else {
                continue;
            };
            if let Some(Some(file)) = files.get(path) {
                if let Some(loc) = file.disk_loc() {
                    if loc.len == size && loc.checksum == body_checksum {
                        self.bodies
                            .entry(String::from(path))
                            .or_default()
                            .offsets[target_slot] = Some(body_offset as usize);
                    }
                }
            }
        }
    }

    pub fn load_history(&mut self) -> Result<VecDeque<String>> {
        let mut bytes = vec![0; HISTORY_RECORD * HISTORY_SLOTS];
        self.store
            .borrow_mut()
            .dev
            .read_blocks(self.history_start(), &mut bytes)
            .map_err(|_| "history read failed")?;
        let mut entries = Vec::new();
        for (slot, record) in bytes.chunks_exact(HISTORY_RECORD).enumerate() {
            if &record[..8] != HISTORY_MAGIC {
                continue;
            }
            let seq = u64::from_le_bytes(record[8..16].try_into().unwrap());
            let len = u16::from_le_bytes(record[16..18].try_into().unwrap()) as usize;
            if seq == 0 || seq % HISTORY_SLOTS as u64 != slot as u64 || !(1..=512).contains(&len) {
                continue;
            }
            let end = 18 + len;
            if record[end..end + 4] != checksum(&record[..end]).to_le_bytes() {
                continue;
            }
            let command = &record[18..end];
            if command.iter().all(|b| (32..=126).contains(b)) {
                entries.push((seq, core::str::from_utf8(command).unwrap().into()));
            }
        }
        entries.sort_unstable_by_key(|(seq, _)| *seq);
        self.history_sequence = Some(entries.last().map_or(0, |entry| entry.0));
        let skip = entries.len().saturating_sub(HISTORY_LIMIT);
        Ok(entries
            .into_iter()
            .skip(skip)
            .map(|(_, cmd)| cmd)
            .collect())
    }

    pub fn append_history(&mut self, command: &str) -> Result<()> {
        let seq = self
            .history_sequence
            .and_then(|n| n.checked_add(1))
            .ok_or("history unavailable")?;
        if !(1..=512).contains(&command.len()) {
            return Err("invalid history entry");
        }
        let mut record = [0u8; HISTORY_RECORD];
        record[..8].copy_from_slice(HISTORY_MAGIC);
        record[8..16].copy_from_slice(&seq.to_le_bytes());
        record[16..18].copy_from_slice(&(command.len() as u16).to_le_bytes());
        let end = 18 + command.len();
        record[18..end].copy_from_slice(command.as_bytes());
        let sum = checksum(&record[..end]);
        record[end..end + 4].copy_from_slice(&sum.to_le_bytes());
        let sector =
            self.history_start() + (seq % HISTORY_SLOTS as u64) as usize * (HISTORY_RECORD / 512);
        let mut store = self.store.borrow_mut();
        store
            .dev
            .write_blocks(sector, &record)
            .and_then(|_| store.dev.flush())
            .map_err(|_| "history write failed")?;
        self.history_sequence = Some(seq);
        Ok(())
    }

    pub(crate) fn invalidate_body(&mut self, path: &str) {
        self.bodies.remove(path);
    }

    pub fn save(&mut self, files: &Files, versions: &Versions) -> Result<()> {
        let result = self.write_snapshot(files, versions);
        if result.is_err() {
            // A partially written inactive slot and a rolled-back VFS edit
            // invalidate both offset and hash information learned during this save.
            self.bodies.clear();
            self.pending_other_idx = None;
        }
        result
    }

    fn write_snapshot(&mut self, files: &Files, versions: &Versions) -> Result<()> {
        let version_data = if versions.is_empty() {
            Vec::new()
        } else {
            serde_json_wasm::to_vec(versions).map_err(|_| "could not save agent versions")?
        };
        if version_data.len() > MAX_VERSIONS {
            return Err("agent version storage full");
        }
        if files
            .values()
            .flatten()
            .any(|bytes| bytes.len() > self.file_limit())
        {
            return Err("file too large");
        }
        let count = files.len();
        let len = 4
            + version_data.len()
            + files
                .iter()
                .map(|(p, v)| 16 + p.len() + v.as_ref().map_or(0, FileData::len))
                .sum::<usize>();
        if len > self.capacity_bytes() {
            return Err("filesystem full");
        }
        let gen_id = self
            .generation
            .checked_add(1)
            .ok_or("snapshot counter full")?;
        let target_slot = gen_id as usize % 2;
        let active_slot = 1 - target_slot;
        let base = SNAPSHOT_BASE + target_slot * self.slot_sectors;
        let active_base = SNAPSHOT_BASE + active_slot * self.slot_sectors + 1;

        // Populate target_slot cached offsets from its prior LXIDX001 before modifying target_slot.
        self.load_pending_other_offsets(target_slot, files);

        let mut store = self.store.borrow_mut();

        // Invalidate target_slot's LXIDX001 extension before overwriting any payload sectors so a
        // crash mid-write can never make a future boot trust stale target_slot offsets.
        if self.slot_has_idx[target_slot] {
            let mut cleared = self.slot_headers[target_slot];
            cleared[28..60].fill(0);
            store
                .dev
                .write_blocks(base, &cleared)
                .and_then(|_| store.dev.flush())
                .map_err(|_| "disk commit failed")?;
            self.slot_headers[target_slot] = cleared;
            self.slot_has_idx[target_slot] = false;
        }

        let mut idx_builder = IndexBuilder::new(count).ok();
        let mut new_locs: Vec<(&FileData, DiskLoc)> = Vec::new();
        new_locs
            .try_reserve(files.len())
            .map_err(|_| "not enough snapshot memory")?;

        let mut read_buf = Vec::new();
        let mut read_start = 0usize;
        let mut read_len = 0usize;
        let active_len = self.snapshot_bytes;

        let mut writer = SnapshotWriter {
            device: &mut store.dev,
            sector: base + 1,
            origin: base + 1,
            chunk: Vec::new(),
            hash: checksum(&[]),
        };
        writer
            .chunk
            .try_reserve_exact(len.next_multiple_of(512).min(IO_CHUNK))
            .map_err(|_| "not enough snapshot memory")?;
        writer.push(&(count as u32).to_le_bytes())?;

        // Helper closure to stream directly from active_slot's 1 MiB read-ahead buffer.
        let ensure_active = |device: &mut VirtIOBlk<Dma, MmioTransport<'static>>,
                             rbuf: &mut Vec<u8>,
                             rstart: &mut usize,
                             rlen: &mut usize,
                             off: usize|
         -> Result<()> {
            if off >= active_len {
                return Err("lazy file out of bounds");
            }
            if rbuf.is_empty() {
                rbuf.try_reserve_exact(IO_CHUNK)
                    .map_err(|_| "not enough snapshot read memory")?;
            }
            if off < *rstart || off >= *rstart + *rlen {
                let aligned = off / 512 * 512;
                let avail = active_len.saturating_sub(aligned);
                let n = avail.next_multiple_of(512).min(IO_CHUNK);
                unsafe { rbuf.set_len(n) };
                device
                    .read_blocks(active_base + aligned / 512, rbuf)
                    .map_err(|_| "disk read failed")?;
                *rstart = aligned;
                *rlen = n.min(avail);
            }
            Ok(())
        };

        for (path, contents) in snapshot_entries(files) {
            let time = self.times.get(path).copied().unwrap_or_default();
            let mut record = [0; 16];
            record[..4].copy_from_slice(&(path.len() as u32).to_le_bytes());
            record[4..8].copy_from_slice(
                &contents
                    .as_ref()
                    .map_or(u32::MAX, |bytes| bytes.len() as u32)
                    .to_le_bytes(),
            );
            record[8..12].copy_from_slice(&time.birth.to_le_bytes());
            record[12..].copy_from_slice(&time.modified.to_le_bytes());
            writer.push(&record)?;
            writer.push(path.as_bytes())?;
            if let Some(file) = contents {
                let file_len = file.len();
                let body_offset = writer.offset();
                let hash_before = writer.hash;
                let body_sum: u32;
                if file_len >= LARGE_BODY {
                    if !self.bodies.contains_key(path) {
                        self.bodies.insert(path.clone(), CachedBody::default());
                    }
                    let cached = self.bodies.get_mut(path).unwrap();
                    if let Some(bytes) = file.loaded() {
                        body_sum = file
                            .disk_loc()
                            .map_or_else(|| checksum(bytes), |l| l.checksum);
                        writer.body_loaded(bytes, cached, target_slot)?;
                    } else {
                        let loc = file.disk_loc().ok_or("missing lazy file location")?;
                        if loc.slot as usize != active_slot || loc.generation != self.generation {
                            return Err("stale lazy file location");
                        }
                        if (loc.offset as usize)
                            .checked_add(file_len)
                            .is_none_or(|e| e > active_len)
                        {
                            return Err("lazy file out of bounds");
                        }
                        body_sum = loc.checksum;
                        let can_skip = cached.offsets[target_slot] == Some(body_offset);
                        if can_skip {
                            let need_hash = match cached.hash {
                                Some((start, end)) if start == hash_before => {
                                    writer.hash = end;
                                    false
                                }
                                _ => true,
                            };
                            if need_hash {
                                let mut rem = file_len;
                                let mut off = loc.offset as usize;
                                let mut h = hash_before;
                                while rem != 0 {
                                    ensure_active(
                                        writer.device,
                                        &mut read_buf,
                                        &mut read_start,
                                        &mut read_len,
                                        off,
                                    )?;
                                    let pos = off - read_start;
                                    let n = rem.min(read_len - pos);
                                    h = hash_bytes(h, &read_buf[pos..pos + n]);
                                    off += n;
                                    rem -= n;
                                }
                                writer.hash = h;
                                cached.hash = Some((hash_before, h));
                            }
                            let prefix = (512 - body_offset % 512) % 512;
                            if prefix != 0 {
                                let mut rem = prefix;
                                let mut off = loc.offset as usize;
                                while rem != 0 {
                                    ensure_active(
                                        writer.device,
                                        &mut read_buf,
                                        &mut read_start,
                                        &mut read_len,
                                        off,
                                    )?;
                                    let pos = off - read_start;
                                    let n = rem.min(read_len - pos);
                                    writer.append(&read_buf[pos..pos + n])?;
                                    off += n;
                                    rem -= n;
                                }
                            }
                            writer.flush_chunk()?;
                            let sectors = (file_len - prefix) / 512;
                            writer.sector += sectors;
                            let suffix = file_len - prefix - sectors * 512;
                            if suffix != 0 {
                                let mut rem = suffix;
                                let mut off = loc.offset as usize + prefix + sectors * 512;
                                while rem != 0 {
                                    ensure_active(
                                        writer.device,
                                        &mut read_buf,
                                        &mut read_start,
                                        &mut read_len,
                                        off,
                                    )?;
                                    let pos = off - read_start;
                                    let n = rem.min(read_len - pos);
                                    writer.append(&read_buf[pos..pos + n])?;
                                    off += n;
                                    rem -= n;
                                }
                            }
                        } else {
                            let hash_hit = match cached.hash {
                                Some((start, end)) if start == hash_before => Some(end),
                                _ => None,
                            };
                            let mut rem = file_len;
                            let mut off = loc.offset as usize;
                            let mut h = hash_before;
                            while rem != 0 {
                                ensure_active(
                                    writer.device,
                                    &mut read_buf,
                                    &mut read_start,
                                    &mut read_len,
                                    off,
                                )?;
                                let pos = off - read_start;
                                let n = rem.min(read_len - pos);
                                let slice = &read_buf[pos..pos + n];
                                if hash_hit.is_none() {
                                    h = hash_bytes(h, slice);
                                }
                                writer.append(slice)?;
                                off += n;
                                rem -= n;
                            }
                            if let Some(end) = hash_hit {
                                writer.hash = end;
                            } else {
                                writer.hash = h;
                                cached.hash = Some((hash_before, h));
                            }
                            cached.offsets[target_slot] = Some(body_offset);
                        }
                    }
                } else if let Some(bytes) = file.loaded() {
                    body_sum = file
                        .disk_loc()
                        .map_or_else(|| checksum(bytes), |l| l.checksum);
                    writer.push(bytes)?;
                } else {
                    let loc = file.disk_loc().ok_or("missing lazy file location")?;
                    if loc.slot as usize != active_slot || loc.generation != self.generation {
                        return Err("stale lazy file location");
                    }
                    if (loc.offset as usize)
                        .checked_add(file_len)
                        .is_none_or(|e| e > active_len)
                    {
                        return Err("lazy file out of bounds");
                    }
                    body_sum = loc.checksum;
                    let mut rem = file_len;
                    let mut off = loc.offset as usize;
                    while rem != 0 {
                        ensure_active(
                            writer.device,
                            &mut read_buf,
                            &mut read_start,
                            &mut read_len,
                            off,
                        )?;
                        let pos = off - read_start;
                        let n = rem.min(read_len - pos);
                        writer.push(&read_buf[pos..pos + n])?;
                        off += n;
                        rem -= n;
                    }
                }
                let hash_after = writer.hash;
                if let Some(builder) = idx_builder.as_mut() {
                    if builder
                        .push(
                            path,
                            file_len as u32,
                            time,
                            body_offset as u32,
                            body_sum,
                            hash_before,
                            hash_after,
                        )
                        .is_err()
                    {
                        idx_builder = None;
                    }
                }
                new_locs.push((
                    file,
                    DiskLoc {
                        slot: target_slot as u8,
                        offset: body_offset as u32,
                        len: file_len as u32,
                        checksum: body_sum,
                        generation: gen_id,
                    },
                ));
            } else if let Some(builder) = idx_builder.as_mut() {
                if builder.push(path, u32::MAX, time, 0, 0, 0, 0).is_err() {
                    idx_builder = None;
                }
            }
        }
        let versions_off = writer.offset();
        writer.push(&version_data)?;
        writer.flush_chunk()?;
        let final_hash = writer.hash;
        drop(writer);
        drop(read_buf);

        let mut header = [0u8; 512];
        header[..8].copy_from_slice(MAGIC_WITH_TIMES);
        header[8..16].copy_from_slice(&gen_id.to_le_bytes());
        header[16..20].copy_from_slice(&(len as u32).to_le_bytes());
        header[20..24].copy_from_slice(&final_hash.to_le_bytes());
        let sum = checksum(&header[..24]);
        header[24..28].copy_from_slice(&sum.to_le_bytes());

        let mut idx_written = false;
        if let Some(builder) = idx_builder {
            let idx = builder.finish();
            let idx_sec_off = len.div_ceil(512);
            let idx_secs = idx.len().div_ceil(512);
            if 1 + idx_sec_off + idx_secs <= self.slot_sectors {
                let meta = IndexMeta {
                    idx_sec_off: idx_sec_off as u32,
                    idx_bytes: idx.len() as u32,
                    idx_checksum: checksum(&idx),
                    versions_off: versions_off as u32,
                    versions_len: version_data.len() as u32,
                };
                write_index_blob(
                    &mut store.dev,
                    target_slot,
                    self.slot_sectors,
                    meta.idx_sec_off,
                    idx,
                )?;
                encode_idx_header(&mut header, meta);
                idx_written = true;
            }
        }

        store
            .dev
            .flush()
            .and_then(|_| store.dev.write_blocks(base, &header))
            .and_then(|_| store.dev.flush())
            .map_err(|_| "disk commit failed")?;

        store.generation = gen_id;
        drop(store);

        // Commit is durable. Point every file's DiskLoc at the newly committed slot.
        for (file, loc) in new_locs {
            file.set_disk_loc(loc);
        }
        self.generation = gen_id;
        self.snapshot_bytes = len;
        self.slot_headers[target_slot] = header;
        self.slot_has_idx[target_slot] = idx_written;
        self.index_scratch(files, len);
        self.revision = self.revision.wrapping_add(1);
        self.scratch_dirty = false;
        #[cfg(feature = "retina")]
        self.report_storage(files, len);
        Ok(())
    }
}
