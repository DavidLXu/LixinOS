//! LixinOS volumes identify files by path. Publish a completed answer using
//! a version check and a filesystem transaction on the guest volume.
use std::ffi::CString;
use std::fs;
use std::io::{self, Write};
use std::os::unix::ffi::OsStrExt;
use std::path::{Component, Path, PathBuf};

pub enum BoundOutput {
    Stream(super::no_follow::BoundOutput),
    File(PathBuf, [u8; 48]),
}

impl BoundOutput {
    pub fn open(path: &Path) -> io::Result<Self> {
        if matches!(path.to_str(), Some("/dev/stdout" | "/dev/stderr")) {
            return super::no_follow::BoundOutput::open(path).map(Self::Stream);
        }
        let leaf = path.as_os_str().as_bytes().rsplit(|&b| b == b'/').next();
        if matches!(leaf, None | Some(b"" | b"." | b"..")) {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "output must name a file",
            ));
        }
        let mut suffix = path.strip_prefix(".").unwrap_or(path);
        let mut base = if suffix.is_absolute() {
            PathBuf::new()
        } else {
            std::env::current_dir()?
        };
        while let Ok(rest) = suffix.strip_prefix("..") {
            base.pop();
            suffix = rest;
        }
        if suffix.components().any(|c| c == Component::ParentDir) {
            return Err(io::Error::other("output path must be normalized"));
        }
        let dest = base.join(suffix);
        if dest.starts_with("/dev") || dest.starts_with("/proc") {
            return Err(io::Error::other("expected an ordinary output file"));
        }
        if !fs::metadata(
            dest.parent()
                .ok_or_else(|| io::Error::other("missing parent"))?,
        )?
        .is_dir()
        {
            return Err(io::Error::other("output parent is not a directory"));
        }
        let name = CString::new(dest.as_os_str().as_bytes())?;
        let mut version = [0; 48];
        unsafe extern "C" {
            fn lx_codex_version(path: *const libc::c_char, version: *mut u8) -> i32;
        }
        // The guest ABI writes exactly 48 bytes and borrows the path only during this call.
        let status = unsafe { lx_codex_version(name.as_ptr(), version.as_mut_ptr()) };
        if status != 0 {
            return Err(io::Error::from_raw_os_error(status));
        }
        Ok(Self::File(dest, version))
    }

    pub fn is_stdout(&self) -> bool {
        matches!(self, Self::Stream(s) if s.is_stdout())
    }

    pub fn write(self, contents: &[u8]) -> io::Result<()> {
        let (dest, version) = match self {
            Self::Stream(s) => return s.write(contents),
            Self::File(dest, version) => (dest, version),
        };
        let mut file = tempfile::NamedTempFile::new_in(dest.parent().unwrap())?;
        file.write_all(contents)?;
        let mut staged = file.into_temp_path(); // close the guest descriptor to commit its bytes
        let from = CString::new(staged.as_os_str().as_bytes())?;
        let to = CString::new(dest.as_os_str().as_bytes())?;
        unsafe extern "C" {
            fn lx_codex_publish(
                from: *const libc::c_char,
                to: *const libc::c_char,
                bytes: *const u8,
                len: usize,
                version: *const u8,
            ) -> i32;
        }
        // Both NUL-terminated paths and the byte slice live throughout this synchronous call.
        let status = unsafe {
            lx_codex_publish(
                from.as_ptr(),
                to.as_ptr(),
                contents.as_ptr(),
                contents.len(),
                version.as_ptr(),
            )
        };
        if status != 0 {
            return Err(io::Error::from_raw_os_error(status));
        }
        // The transaction moved the temporary file; avoid unlinking a subsequently reused name.
        staged.disable_cleanup(true);
        Ok(())
    }
}
