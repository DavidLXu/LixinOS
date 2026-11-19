//! Private heap-backed copies of compiler inputs on LixinOS. The larger,
//! compressed sysroot metadata uses the process's second non-executable arena.
use std::collections::HashMap;
use std::fs::File;
use std::io::{self, BufRead, BufReader, Read};
use std::ops::{Deref, DerefMut};
use std::path::{Path, PathBuf};
use std::ptr::NonNull;
use std::sync::{Arc, LazyLock, Mutex, Weak};
use std::slice;

unsafe extern "C" {
    fn lx_rmeta_alloc(len: usize) -> *mut u8;
    fn lx_rmeta_free(ptr: *mut u8, len: usize);
}

pub struct Metadata {
    ptr: NonNull<u8>,
    len: usize,
}

// Metadata is private to this allocation and immutable after decompression.
unsafe impl Send for Metadata {}
unsafe impl Sync for Metadata {}

impl Drop for Metadata {
    fn drop(&mut self) {
        unsafe { lx_rmeta_free(self.ptr.as_ptr(), self.len) };
    }
}

pub enum Mmap {
    Bytes(Vec<u8>),
    Metadata(Arc<Metadata>),
}

impl Mmap {
    // rustc may open the same core metadata both as a transitive dependency
    // and by name. Keep one decompressed copy while either reader is alive.
    pub fn map_metadata(path: &Path, file: File) -> io::Result<Self> {
        static CACHE: LazyLock<Mutex<HashMap<PathBuf, Weak<Metadata>>>> =
            LazyLock::new(|| Mutex::new(HashMap::new()));
        if path.starts_with("/lib/rust/lib/rustlib") {
            let mut cache = CACHE.lock().unwrap();
            if let Some(data) = cache.get(path).and_then(Weak::upgrade) {
                return Ok(Self::Metadata(data));
            }
            let mapping = unsafe { Self::map(file) }?;
            if let Self::Metadata(data) = &mapping {
                cache.insert(path.to_owned(), Arc::downgrade(data));
            }
            return Ok(mapping);
        }
        unsafe { Self::map(file) }
    }

    pub unsafe fn map(file: File) -> io::Result<Self> {
        let mut reader = BufReader::new(file);
        if reader.fill_buf()?.starts_with(b"LXRMETA1") {
            let mut header = [0; 12];
            reader.read_exact(&mut header)?;
            let len = u32::from_le_bytes(header[8..].try_into().unwrap()) as usize;
            if !(1..=100 << 20).contains(&len) { return Err(io::ErrorKind::InvalidData.into()); }
            let ptr = NonNull::new(unsafe { lx_rmeta_alloc(len) }).ok_or(io::ErrorKind::OutOfMemory)?;
            let data = Metadata { ptr, len };
            let mut decoder = flate2::bufread::ZlibDecoder::new(reader);
            decoder.read_exact(unsafe { slice::from_raw_parts_mut(ptr.as_ptr(), len) })?;
            if decoder.read(&mut [0])? != 0 { return Err(io::ErrorKind::InvalidData.into()); }
            Ok(Self::Metadata(Arc::new(data)))
        } else {
            let len = usize::try_from(reader.get_ref().metadata()?.len())
                .map_err(|_| io::ErrorKind::InvalidData)?;
            let mut data = vec![0; len];
            reader.read_exact(&mut data)?;
            Ok(Self::Bytes(data))
        }
    }
}

impl Deref for Mmap {
    type Target = [u8];
    fn deref(&self) -> &[u8] {
        match self {
            Self::Bytes(v) => v,
            Self::Metadata(m) => unsafe { slice::from_raw_parts(m.ptr.as_ptr(), m.len) },
        }
    }
}

impl AsRef<[u8]> for Mmap { fn as_ref(&self) -> &[u8] { self } }

pub struct MmapMut(Vec<u8>);
impl MmapMut {
    pub fn map_anon(len: usize) -> io::Result<Self> { Ok(Self(vec![0; len])) }
    pub fn flush(&mut self) -> io::Result<()> { Ok(()) }
    pub fn make_read_only(self) -> io::Result<Mmap> { Ok(Mmap::Bytes(self.0)) }
}
impl Deref for MmapMut {
    type Target = [u8];
    fn deref(&self) -> &[u8] { &self.0 }
}
impl DerefMut for MmapMut { fn deref_mut(&mut self) -> &mut [u8] { &mut self.0 } }
