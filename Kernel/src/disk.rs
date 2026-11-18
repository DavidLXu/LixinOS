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
type Result<T> = core::result::Result<T, &'static str>;
const LARGE_SLOT: usize = 64 * 1024 * 1024 / 512;
const LARGE_MAX: usize = (LARGE_SLOT - 1) * 512;
const SNAPSHOT_BASE: usize = 1; // Sector zero identifies the disk layout.
// Leave room for directories and system files alongside one million user files.
pub const ENTRY_LIMIT: usize = 1_048_576;
pub const FILE_LIMIT: usize = 1024 * 1024;
const MAX_VERSIONS: usize = 1024 * 1024;
const MAGIC: &[u8; 8] = b"LIXINFS1";
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
    generation: u64,
    history_sequence: Option<u64>,
}

pub fn file_counts(files: &Files) -> (usize, usize, usize) {
    let file_count = files.values().flatten().count();
    let content_bytes = files.values().flatten().map(Vec::len).sum();
    (file_count, files.len() - file_count, content_bytes)
}

pub fn resolve_path(files: &Files, cwd: &str, name: &str) -> Result<String> {
    if name.is_empty() {
        return Err("empty path");
    }
    let mut path = if name.starts_with('/') {
        String::new()
    } else {
        cwd.into()
    };
    for part in name.split('/').filter(|p| !p.is_empty()) {
        if !path.is_empty() && files.get(&path) != Some(&None) {
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
    children_after(files, path, None)
}

pub fn children_after<'a>(files: &'a Files, path: &str, after: Option<&str>)
    -> impl Iterator<Item = (&'a str, &'a Option<Vec<u8>>)> {
    use core::ops::Bound::{Excluded, Included, Unbounded};
    let prefix = alloc::format!("{}/", path.trim_end_matches('/'));
    let start = after.map_or(Included(prefix.as_str()), Excluded);
    files.range::<str, _>((start, Unbounded))
        .map_while(move |(path, value)| Some((path.strip_prefix(&prefix)?, value)))
        .filter(|(name, _)| !name.is_empty() && !name.contains('/'))
}

pub(crate) fn checksum(bytes: &[u8]) -> u32 {
    bytes.iter().fold(2166136261, |h, b| {
        (h ^ u32::from(*b)).wrapping_mul(16777619)
    })
}
fn take<'a>(src: &mut &'a [u8], n: usize) -> Result<&'a [u8]> {
    let (first, rest) = src.split_at_checked(n).ok_or("invalid filesystem record")?;
    *src = rest;
    Ok(first)
}
fn number(src: &mut &[u8]) -> Result<u32> {
    Ok(u32::from_le_bytes(take(src, 4)?.try_into().unwrap()))
}
fn decode(mut src: &[u8], has_versions: bool) -> Result<(Files, Versions)> {
    let count = number(&mut src)? as usize;
    if count > ENTRY_LIMIT {
        return Err("invalid filesystem size");
    }
    let mut files = Files::new();
    for _ in 0..count {
        let n = number(&mut src)? as usize;
        let size = number(&mut src)?;
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
        if files.insert(path.into(), value).is_some() {
            return Err("duplicate path");
        }
    }
    if files.get("/") != Some(&None) {
        return Err("invalid filesystem");
    }
    let versions = if has_versions {
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
    Ok((files, versions))
}
impl Disk {
    pub fn snapshot_status(&self) -> (u64, u64) {
        (self.dev.capacity(), self.generation)
    }

    #[cfg(feature = "retina")]
    fn report_storage(&self, files: &Files, payload_bytes: usize) {
        let capacity = LARGE_MAX;
        crate::graphics::set_storage_usage(files.values().flatten().count(), payload_bytes, capacity);
    }

    pub fn mount() -> Result<(Self, Files, Versions)> {
        // Launcher connects the disk to virtio-mmio-bus.0 on QEMU virt.
        let transport =
            unsafe { MmioTransport::new(NonNull::new(0x0a00_0000 as *mut _).unwrap(), 512) }
                .map_err(|_| "no virtio disk")?;
        let mut dev = VirtIOBlk::new(transport).map_err(|_| "disk initialization failed")?;
        if dev.capacity() < DISK_SECTORS as u64 { return Err("disk too small for LIXDISK3"); }
        let mut layout = [0u8; 512];
        dev.read_blocks(0, &mut layout).map_err(|_| "disk read failed")?;
        if &layout[..8] != b"LIXDISK3" || layout[8..].iter().any(|b| *b != 0) {
            return Err("unsupported disk layout");
        }
        let mut disk = Self { dev, generation: 0, history_sequence: None };
        if let Some((files, versions)) = disk.read_pair()? {
            return Ok((disk, files, versions));
        }
        let files = Files::from([("/".into(), None)]);
        let versions = Versions::new();
        disk.save(&files, &versions)?;
        Ok((disk, files, versions))
    }
    fn read_pair(&mut self) -> Result<Option<(Files, Versions)>> {
        let mut heads = [[0u8; 512]; 2];
        for (slot, h) in heads.iter_mut().enumerate() {
            self.dev
                .read_blocks(SNAPSHOT_BASE + slot * LARGE_SLOT, h)
                .map_err(|_| "disk read failed")?;
        }
        let generation = |h: &[u8; 512]| u64::from_le_bytes(h[8..16].try_into().unwrap());
        let newest = usize::from(generation(&heads[1]) > generation(&heads[0]));
        for slot in [newest, 1 - newest] {
            let h = &heads[slot];
            let has_versions = &h[..8] == MAGIC_WITH_VERSIONS;
            if (!has_versions && &h[..8] != MAGIC) || checksum(&h[..24]).to_le_bytes() != h[24..28] {
                continue;
            }
            let len = u32::from_le_bytes(h[16..20].try_into().unwrap()) as usize;
            if !(4..=LARGE_MAX).contains(&len) {
                continue;
            }
            let mut payload = vec![0; len.next_multiple_of(512)];
            self.dev
                .read_blocks(SNAPSHOT_BASE + slot * LARGE_SLOT + 1, &mut payload)
                .map_err(|_| "disk read failed")?;
            if checksum(&payload[..len]).to_le_bytes() != h[20..24] {
                continue;
            }
            if let Ok((files, versions)) = decode(&payload[..len], has_versions) {
                self.generation = generation(h);
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
            .read_blocks(HISTORY_START, &mut bytes)
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
        let sector = HISTORY_START + (seq % HISTORY_SLOTS as u64) as usize * (HISTORY_RECORD / 512);
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
        if files.values().flatten().any(|bytes| bytes.len() > FILE_LIMIT) {
            return Err("file too large");
        }
        let len = 4 + version_data.len() + files
            .iter()
            .map(|(p, v)| 8 + p.len() + v.as_ref().map_or(0, Vec::len))
            .sum::<usize>();
        if len > LARGE_MAX { return Err("filesystem full"); }
        let mut data = Vec::with_capacity(len.next_multiple_of(512));
        data.extend((files.len() as u32).to_le_bytes());
        for (p, v) in files {
            data.extend((p.len() as u32).to_le_bytes());
            data.extend(
                v.as_ref()
                    .map_or(u32::MAX, |s| s.len() as u32)
                    .to_le_bytes(),
            );
            data.extend(p.as_bytes());
            if let Some(s) = v {
                data.extend_from_slice(s)
            }
        }
        data.extend_from_slice(&version_data);
        let gen_id = self
            .generation
            .checked_add(1)
            .ok_or("snapshot counter full")?;
        let mut header = [0u8; 512];
        let magic = if versions.is_empty() {
            MAGIC
        } else {
            MAGIC_WITH_VERSIONS
        };
        header[..8].copy_from_slice(magic);
        header[8..16].copy_from_slice(&gen_id.to_le_bytes());
        header[16..20].copy_from_slice(&(len as u32).to_le_bytes());
        header[20..24].copy_from_slice(&checksum(&data).to_le_bytes());
        let sum = checksum(&header[..24]);
        header[24..28].copy_from_slice(&sum.to_le_bytes());
        data.resize(len.next_multiple_of(512), 0);
        let base = SNAPSHOT_BASE + (gen_id as usize % 2) * LARGE_SLOT;
        self.dev
            .write_blocks(base + 1, &data)
            .and_then(|_| self.dev.flush())
            .and_then(|_| self.dev.write_blocks(base, &header))
            .and_then(|_| self.dev.flush())
            .map_err(|_| "disk commit failed")?;
        self.generation = gen_id;
        #[cfg(feature = "retina")]
        self.report_storage(files, len);
        Ok(())
    }
}
