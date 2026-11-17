//! Two alternating snapshots on a dedicated virtio-blk disk. A flushed header
//! publishes its payload only after all payload sectors have reached the disk.
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
const SLOT: usize = 8192; // 4 MiB in 512-byte sectors
pub const ENTRY_LIMIT: usize = 128;
pub const FILE_LIMIT: usize = 16 * 1024;
const MAX: usize = ENTRY_LIMIT * (512 + FILE_LIMIT + 8) + 4;
const MAGIC: &[u8; 8] = b"LIXINFS1";
// The filesystem snapshots use only the front of each slot. Keep command
// history at the end of the second slot, including one spare record so a
// torn append cannot overwrite any of the 64 commands still in history.
pub const HISTORY_LIMIT: usize = 64;
const HISTORY_RECORD: usize = 1024;
const HISTORY_SLOTS: usize = HISTORY_LIMIT + 1;
const HISTORY_START: usize = 2 * SLOT - HISTORY_SLOTS * (HISTORY_RECORD / 512);
const _: () = assert!(SLOT + 1 + MAX.div_ceil(512) <= HISTORY_START);
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

pub fn children<'a>(files: &'a Files, path: &str) -> impl Iterator<Item = (&'a str, &'a Option<Vec<u8>>)> {
    let prefix = alloc::format!("{}/", path.trim_end_matches('/'));
    files
        .range(prefix.clone()..)
        .map_while(move |(path, value)| Some((path.strip_prefix(&prefix)?, value)))
        .filter(|(name, _)| !name.is_empty() && !name.contains('/'))
}
fn checksum(bytes: &[u8]) -> u32 {
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
fn decode(mut src: &[u8]) -> Result<Files> {
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
    if !src.is_empty() || files.get("/") != Some(&None) {
        return Err("invalid filesystem");
    }
    Ok(files)
}
impl Disk {
    pub fn snapshot_status(&self) -> (u64, u64) {
        (self.dev.capacity(), self.generation)
    }

    pub fn mount() -> Result<(Self, Files)> {
        // Launcher connects the disk to virtio-mmio-bus.0 on QEMU virt.
        let transport =
            unsafe { MmioTransport::new(NonNull::new(0x0a00_0000 as *mut _).unwrap(), 512) }
                .map_err(|_| "no virtio disk")?;
        let dev = VirtIOBlk::new(transport).map_err(|_| "disk initialization failed")?;
        if dev.capacity() < 2 * SLOT as u64 {
            return Err("disk requires 8 MiB");
        }
        let mut disk = Self {
            dev,
            generation: 0,
            history_sequence: None,
        };
        let mut heads = [[0u8; 512]; 2];
        for (slot, h) in heads.iter_mut().enumerate() {
            disk.dev
                .read_blocks(slot * SLOT, h)
                .map_err(|_| "disk read failed")?;
        }
        let generation = |h: &[u8; 512]| u64::from_le_bytes(h[8..16].try_into().unwrap());
        let newest = usize::from(generation(&heads[1]) > generation(&heads[0]));
        for slot in [newest, 1 - newest] {
            let h = &heads[slot];
            if &h[..8] != MAGIC || checksum(&h[..24]).to_le_bytes() != h[24..28] {
                continue;
            }
            let len = u32::from_le_bytes(h[16..20].try_into().unwrap()) as usize;
            if !(4..=MAX).contains(&len) {
                continue;
            }
            let mut payload = vec![0; len.next_multiple_of(512)];
            disk.dev
                .read_blocks(slot * SLOT + 1, &mut payload)
                .map_err(|_| "disk read failed")?;
            if checksum(&payload[..len]).to_le_bytes() != h[20..24] {
                continue;
            }
            if let Ok(files) = decode(&payload[..len]) {
                disk.generation = generation(h);
                return Ok((disk, files));
            }
        }
        if heads.iter().flatten().any(|b| *b != 0) {
            return Err("no valid filesystem snapshot");
        }
        let files = Files::from([("/".into(), None)]);
        disk.save(&files)?;
        Ok((disk, files))
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

    pub fn save(&mut self, files: &Files) -> Result<()> {
        let len = 4 + files
            .iter()
            .map(|(p, v)| 8 + p.len() + v.as_ref().map_or(0, Vec::len))
            .sum::<usize>();
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
        let gen_id = self
            .generation
            .checked_add(1)
            .ok_or("snapshot counter full")?;
        let mut header = [0u8; 512];
        header[..8].copy_from_slice(MAGIC);
        header[8..16].copy_from_slice(&gen_id.to_le_bytes());
        header[16..20].copy_from_slice(&(len as u32).to_le_bytes());
        header[20..24].copy_from_slice(&checksum(&data).to_le_bytes());
        let sum = checksum(&header[..24]);
        header[24..28].copy_from_slice(&sum.to_le_bytes());
        data.resize(len.next_multiple_of(512), 0);
        let base = (gen_id as usize % 2) * SLOT;
        self.dev
            .write_blocks(base + 1, &data)
            .and_then(|_| self.dev.flush())
            .and_then(|_| self.dev.write_blocks(base, &header))
            .and_then(|_| self.dev.flush())
            .map_err(|_| "disk commit failed")?;
        self.generation = gen_id;
        Ok(())
    }
}
