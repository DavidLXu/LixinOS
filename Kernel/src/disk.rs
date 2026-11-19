//! Two alternating snapshots on a dedicated virtio-blk disk. A flushed header
//! publishes its payload only after all payload sectors have reached the disk.
use crate::agent::Versions;
use alloc::{
    alloc::{Layout, alloc_zeroed, dealloc, handle_alloc_error},
    collections::{BTreeMap, VecDeque},
    string::String,
    vec,
    vec::Vec,
};
use core::ptr::NonNull;
use virtio_drivers::{
    BufferDirection, Hal, PhysAddr, device::blk::VirtIOBlk, transport::mmio::MmioTransport,
};

pub type Files = BTreeMap<String, Option<Vec<u8>>>;
pub(crate) type Times = BTreeMap<String, FileTime>;
#[derive(Clone, Copy, Default)]
pub(crate) struct FileTime {
    pub birth: u32,
    pub modified: u32,
}

type Result<T> = core::result::Result<T, &'static str>;
const LARGE_SLOT: usize = 192 * 1024 * 1024 / 512;
const BUILD_SLOT: usize = (4usize << 30) / 512;
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

pub struct Disk {
    dev: VirtIOBlk<Dma, MmioTransport<'static>>,
    slot_sectors: usize,
    pub(crate) scratch_dirty: bool,
    live_bytes: usize,
    revision: u64,
    scratch_records: BTreeMap<String, usize>,
    generation: u64,
    snapshot_bytes: usize,
    history_sequence: Option<u64>,
    pub(crate) times: Times,
}

pub fn file_counts(files: &Files) -> (usize, usize, usize) {
    let file_count = files.values().flatten().count();
    let content_bytes = files.values().flatten().map(Vec::len).sum();
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

pub fn children<'a>(files: &'a Files, path: &str) -> impl Iterator<Item = (&'a str, &'a Option<Vec<u8>>)> {
    children_matching(files, path, "", None)
}

pub fn children_matching<'a>(files: &'a Files, path: &str, prefix: &str, after: Option<&str>)
    -> impl Iterator<Item = (&'a str, &'a Option<Vec<u8>>)> {
    use core::ops::Bound::{Excluded, Included, Unbounded};
    let base = alloc::format!("{}/", path.trim_end_matches('/'));
    let prefix_key = alloc::format!("{base}{prefix}");
    let start = after.filter(|name| *name >= prefix)
        .map_or_else(|| Included(prefix_key.clone()), |name| Excluded(alloc::format!("{base}{name}")));
    files.range((start, Unbounded))
        .take_while(move |(path, _)| path.starts_with(&prefix_key))
        .map(move |(path, value)| (&path[base.len()..], value))
        .filter(|(name, _)| !name.is_empty() && !name.contains('/'))
}

fn hash_bytes(hash: u32, bytes: &[u8]) -> u32 {
    bytes.iter().fold(hash, |hash, byte| {
        (hash ^ u32::from(*byte)).wrapping_mul(16777619)
    })
}
pub(crate) fn checksum(bytes: &[u8]) -> u32 {
    hash_bytes(2166136261, bytes)
}

struct SnapshotWriter<'a> {
    device: &'a mut VirtIOBlk<Dma, MmioTransport<'static>>,
    sector: usize,
    chunk: Vec<u8>,
    hash: u32,
}

impl SnapshotWriter<'_> {
    fn push(&mut self, mut bytes: &[u8]) -> Result<()> {
        self.hash = hash_bytes(self.hash, bytes);
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
        if self.chunk.is_empty() { return Ok(()); }
        self.chunk.resize(self.chunk.len().next_multiple_of(512), 0);
        self.device.write_blocks(self.sector, &self.chunk).map_err(|_| "disk commit failed")?;
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
            FileTime { birth: number(&mut src)?, modified: number(&mut src)? }
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
            Some(take(&mut src, size as usize)?.to_vec())
        };
        if time.birth != 0 || time.modified != 0 { times.insert(path.into(), time); }
        if files.insert(path.into(), value).is_some() {
            return Err("duplicate path");
        }
    }
    if files.get("/") != Some(&None) {
        return Err("invalid filesystem");
    }
    let versions = if format == MAGIC_WITH_VERSIONS || (format == MAGIC_WITH_TIMES && !src.is_empty()) {
        if src.len() > MAX_VERSIONS {
            return Err("invalid agent version data size");
        }
        let versions: Versions =
            serde_json_wasm::from_slice(src).map_err(|_| "invalid agent version data")?;
        if versions.iter().any(|(path, version)| {
            !path.ends_with(".py") || !files.get(path).is_some_and(Option::is_some) || !version.valid()
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
impl Disk {
    pub(crate) fn capacity_bytes(&self) -> usize { (self.slot_sectors - 1) * 512 }
    pub(crate) fn scratch_path(&self, path: &str) -> bool {
        self.slot_sectors > LARGE_SLOT && path.starts_with("/scratch/")
    }
    fn history_start(&self) -> usize { SNAPSHOT_BASE + 2 * self.slot_sectors }
    pub(crate) fn file_limit(&self) -> usize {
        if self.slot_sectors > LARGE_SLOT { FILE_LIMIT } else { SYSTEM_FILE_LIMIT }
    }

    pub(crate) fn write_scratch(&mut self, path: &str, file: &mut Vec<u8>, start: usize, data: &[u8]) -> Result<()> {
        let end = start.checked_add(data.len()).ok_or("file too large")?;
        let additional = end.saturating_sub(file.len());
        if end > self.file_limit() || self.live_bytes + additional > self.capacity_bytes() {
            return Err("filesystem full");
        }
        let record = self.scratch_records.get_mut(path).ok_or("missing scratch file")?;
        file.try_reserve(additional).map_err(|_| "not enough memory")?;
        file.resize(file.len() + additional, 0);
        file[start..end].copy_from_slice(data);
        *record += additional;
        self.live_bytes += additional;
        self.times.entry(String::from(path)).or_default().modified = crate::utility::wall_seconds();
        self.revision = self.revision.wrapping_add(1);
        self.scratch_dirty = true;
        Ok(())
    }

    pub(crate) fn used_bytes(&self) -> usize { self.live_bytes }
    pub(crate) fn revision(&self) -> u64 { self.revision }

    fn record_bytes(path: &str, entry: &Option<Vec<u8>>) -> usize {
        16 + path.len() + entry.as_ref().map_or(0, Vec::len)
    }

    fn index_scratch(&mut self, files: &Files, len: usize) {
        self.live_bytes = len;
        if self.slot_sectors > LARGE_SLOT {
            self.scratch_records = files.iter().filter(|(path, _)| self.scratch_path(path))
                .map(|(path, entry)| (path.clone(), Self::record_bytes(path, entry))).collect();
        }
    }

    // Admit the transaction only if the resulting tree can still be checkpointed.
    pub(crate) fn write_back(&mut self, files: &Files, paths: &[(&str, Option<FileTime>)]) -> Result<()> {
        let mut records = BTreeMap::new();
        for &(path, _) in paths {
            if self.scratch_path(path) {
                let size = files.get(path).map_or(0, |entry| Self::record_bytes(path, entry));
                records.insert(path, size);
            }
        }
        let old: usize = records.keys().filter_map(|p| self.scratch_records.get(*p)).sum();
        let len = self.live_bytes - old + records.values().sum::<usize>();
        if len > self.capacity_bytes() { return Err("filesystem full"); }
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
        (self.dev.capacity(), self.generation)
    }

    // The mounted tree is ordered exactly like the snapshot. Read the selected
    // record back from the device without reloading the rest of the volume.
    pub(crate) fn verify_file(&mut self, files: &Files, path: &str) -> Result<u64> {
        let bytes = files.get(path).and_then(Option::as_deref).ok_or("saved file not found")?;
        let base = SNAPSHOT_BASE + (self.generation as usize % 2) * self.slot_sectors;
        let mut header = [0u8; 512];
        self.dev.read_blocks(base, &mut header).map_err(|_| "snapshot read failed")?;
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
        if self.scratch_dirty { return Err("build filesystem has pending writes"); }
        let offset = 4 + files.iter().take_while(|(name, _)| name.as_str() < path)
            .map(|(name, entry)| record_header + name.len() + entry.as_ref().map_or(0, Vec::len))
            .sum::<usize>();
        let end = offset + record_header + path.len() + bytes.len();
        if end > self.snapshot_bytes { return Err("file record is outside the committed snapshot"); }
        let start = offset / 512 * 512;
        let mut window = vec![0; end.next_multiple_of(512) - start];
        self.dev.read_blocks(base + 1 + start / 512, &mut window)
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
        crate::graphics::set_storage_usage(files.values().flatten().count(), payload_bytes, capacity);
    }

    pub fn mount() -> Result<(Self, Files, Versions)> {
        // Launcher connects the disk to virtio-mmio-bus.0 on QEMU virt.
        let transport =
            unsafe { MmioTransport::new(NonNull::new(0x0a00_0000 as *mut _).unwrap(), 512) }
                .map_err(|_| "no virtio disk")?;
        let mut dev = VirtIOBlk::new(transport).map_err(|_| "disk initialization failed")?;
        if dev.capacity() < DISK_SECTORS as u64 { return Err("disk too small for LIXDISK4"); }
        let mut layout = [0u8; 512];
        dev.read_blocks(0, &mut layout).map_err(|_| "disk read failed")?;
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
            || layout[8..].iter().any(|b| *b != 0) {
            return Err("unsupported disk layout");
        }
        let mut disk = Self { dev, slot_sectors, scratch_dirty: false, scratch_records: BTreeMap::new(), live_bytes: 0, revision: 0, generation: 0, snapshot_bytes: 0, history_sequence: None, times: Times::new() };
        if let Some((files, versions)) = disk.read_pair()? {
            return Ok((disk, files, versions));
        }
        let files = Files::from([("/".into(), None)]);
        let now = crate::utility::wall_seconds();
        disk.times.insert("/".into(), FileTime { birth: now, modified: now });
        let versions = Versions::new();
        disk.save(&files, &versions)?;
        Ok((disk, files, versions))
    }
    fn read_pair(&mut self) -> Result<Option<(Files, Versions)>> {
        let mut heads = [[0u8; 512]; 2];
        for (slot, h) in heads.iter_mut().enumerate() {
            self.dev
                .read_blocks(SNAPSHOT_BASE + slot * self.slot_sectors, h)
                .map_err(|_| "disk read failed")?;
        }
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
            let mut payload = vec![0; len.next_multiple_of(512)];
            let base = SNAPSHOT_BASE + slot * self.slot_sectors + 1;
            for (index, chunk) in payload.chunks_mut(IO_CHUNK).enumerate() {
                self.dev.read_blocks(base + index * (IO_CHUNK / 512), chunk)
                    .map_err(|_| "disk read failed")?;
            }
            if checksum(&payload[..len]).to_le_bytes() != h[20..24] {
                continue;
            }
            if let Ok((files, versions, times)) = decode(&payload[..len], format) {
                self.times = times;
                self.generation = generation(h);
                self.snapshot_bytes = len;
                self.revision = self.generation;
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

    pub fn load_history(&mut self) -> Result<VecDeque<String>> {
        let mut bytes = vec![0; HISTORY_RECORD * HISTORY_SLOTS];
        self.dev
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
        Ok(entries.into_iter().skip(skip).map(|(_, cmd)| cmd).collect())
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
        let sector = self.history_start() + (seq % HISTORY_SLOTS as u64) as usize * (HISTORY_RECORD / 512);
        self.dev
            .write_blocks(sector, &record)
            .and_then(|_| self.dev.flush())
            .map_err(|_| "history write failed")?;
        self.history_sequence = Some(seq);
        Ok(())
    }

    pub fn save(&mut self, files: &Files, versions: &Versions) -> Result<()> {
        let version_data = if versions.is_empty() {
            Vec::new()
        } else {
            serde_json_wasm::to_vec(versions).map_err(|_| "could not save agent versions")?
        };
        if version_data.len() > MAX_VERSIONS {
            return Err("agent version storage full");
        }
        if files.values().flatten().any(|bytes| bytes.len() > self.file_limit()) {
            return Err("file too large");
        }
        let count = files.len();
        let len = 4 + version_data.len() + files.iter()
            .map(|(p, v)| 16 + p.len() + v.as_ref().map_or(0, Vec::len))
            .sum::<usize>();
        if len > self.capacity_bytes() { return Err("filesystem full"); }
        let gen_id = self.generation.checked_add(1).ok_or("snapshot counter full")?;
        let base = SNAPSHOT_BASE + (gen_id as usize % 2) * self.slot_sectors;
        let mut writer = SnapshotWriter {
            device: &mut self.dev,
            sector: base + 1,
            chunk: Vec::new(),
            hash: checksum(&[]),
        };
        writer.chunk.try_reserve_exact(len.next_multiple_of(512).min(IO_CHUNK))
            .map_err(|_| "not enough snapshot memory")?;
        writer.push(&(count as u32).to_le_bytes())?;
        let mut timestamps = self.times.iter().peekable();
        for (path, contents) in files {
            let time = if timestamps.peek().is_some_and(|(name, _)| *name == path) {
                *timestamps.next().unwrap().1
            } else {
                FileTime::default()
            };
            let mut record = [0; 16];
            record[..4].copy_from_slice(&(path.len() as u32).to_le_bytes());
            record[4..8].copy_from_slice(&contents.as_ref().map_or(u32::MAX, |bytes| bytes.len() as u32).to_le_bytes());
            record[8..12].copy_from_slice(&time.birth.to_le_bytes());
            record[12..].copy_from_slice(&time.modified.to_le_bytes());
            writer.push(&record)?;
            writer.push(path.as_bytes())?;
            if let Some(bytes) = contents { writer.push(bytes)?; }
        }
        writer.push(&version_data)?;
        writer.flush_chunk()?;
        let mut header = [0u8; 512];
        header[..8].copy_from_slice(MAGIC_WITH_TIMES);
        header[8..16].copy_from_slice(&gen_id.to_le_bytes());
        header[16..20].copy_from_slice(&(len as u32).to_le_bytes());
        header[20..24].copy_from_slice(&writer.hash.to_le_bytes());
        let sum = checksum(&header[..24]);
        header[24..28].copy_from_slice(&sum.to_le_bytes());
        self.dev.flush()
            .and_then(|_| self.dev.write_blocks(base, &header))
            .and_then(|_| self.dev.flush())
            .map_err(|_| "disk commit failed")?;
        self.generation = gen_id;
        self.snapshot_bytes = len;
        self.index_scratch(files, len);
        self.revision = self.revision.wrapping_add(1);
        self.scratch_dirty = false;
        #[cfg(feature = "retina")]
        self.report_storage(files, len);
        Ok(())
    }
}
