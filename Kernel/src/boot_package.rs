// Optional first-boot userland supplied via QEMU fw_cfg. Installed files live on disk.
use crate::disk::{self, Files};
use alloc::vec::Vec;
use core::ptr::{addr_of, addr_of_mut, read_volatile, write_volatile};

fn cpython() -> Result<Files, &'static str> {
    let select = 0x0902_0008 as *mut u16;
    let read = || unsafe { read_volatile(0x0902_0000 as *const u8) };
    unsafe {
        write_volatile(select, 0x19u16.to_be());
    }
    let count = u32::from_be_bytes(core::array::from_fn(|_| read()));
    for _ in 0..count.min(u16::MAX.into()) {
        let entry: [u8; 64] = core::array::from_fn(|_| read());
        if !entry[8..].starts_with(b"opt/lixinos/cpython\0") {
            continue;
        }
        let size = u32::from_be_bytes(entry[..4].try_into().unwrap());
        if !(4..=32 << 20).contains(&size) {
            return Err("invalid CPython package size");
        }
        let id = u16::from_be_bytes([entry[4], entry[5]]);
        let mut payload = Vec::new();
        payload
            .try_reserve_exact(size as usize)
            .map_err(|_| "not enough memory to install CPython")?;
        payload.resize(size as usize, 0u8);
        #[repr(C, align(8))]
        struct Transfer {
            control: u32,
            length: u32,
            address: u64,
        }
        let mut transfer = Transfer {
            control: ((u32::from(id) << 16) | 8 | 2).to_be(), // SELECT | READ
            length: size.to_be(),
            address: (payload.as_mut_ptr() as u64).to_be(),
        };
        unsafe {
            core::arch::asm!("dsb sy");
            write_volatile(
                0x0902_0010 as *mut u64,
                (addr_of_mut!(transfer) as u64).to_be(),
            );
            while u32::from_be(read_volatile(addr_of!(transfer.control))) & !1 != 0 {
                core::hint::spin_loop();
            }
            core::arch::asm!("dsb sy");
            if read_volatile(addr_of!(transfer.control)) != 0 {
                return Err("CPython package read failed");
            }
        }
        let (files, _, _) = disk::decode(&payload, b"LIXINFS1")?;
        if !files.get("/bin/python3").is_some_and(Option::is_some)
            || !files.keys().all(|p| {
                matches!(
                    p.as_str(),
                    "/" | "/bin"
                        | "/lib"
                        | "/bin/python3"
                        | "/bin/pip"
                        | "/bin/pip3"
                        | "/tmp"
                        | "/lib/python3.14"
                        | "/lib/cpython-3.14.5"
                ) || p.starts_with("/lib/python3.14/")
                    || p.starts_with("/lib/cpython-3.14.5/")
            })
        {
            return Err("invalid CPython package paths");
        }
        return Ok(files);
    }
    Ok(Files::new())
}

// Only replace files whose contents match an earlier shipped copy.
fn previous_release(path: &str, bytes: &[u8]) -> bool {
    let signature = (bytes.len(), disk::checksum(bytes));
    match path {
        "/bin/python3" => matches!(signature,
            (6056, 0xfc56f810) | (6312, 0x7d2ee280) | (6312, 0x3956f4ff) | (6968, 0x547b_29fb)),
        "/bin/pip" | "/bin/pip3" => matches!(signature,
            (6336, 0x29d949c6) | (6336, 0xcf019ea3) | (6992, 0x49af_8c3f)),
        "/lib/python3.14/subprocess.py" => signature == (90732, 0xb73b5f73),
        _ => false,
    }
}

pub fn install_cpython(volume: &mut crate::process::Volume) -> Result<(), &'static str> {
    if let Some(existing) = volume.files.get("/bin/python3") {
        if !existing.as_deref().is_some_and(|b| previous_release("/bin/python3", b)) {
            return Ok(());
        }
    }
    let mut package = cpython()?;
    package.retain(|path, contents| volume.files.get(path) != Some(contents));
    for path in package.keys() {
        if let Some(existing) = volume.files.get(path) {
            if !existing.as_deref().is_some_and(|b| previous_release(path, b)) {
                return Err("an existing file conflicts with the CPython package");
            }
        }
    }
    if package.is_empty() {
        return Ok(());
    }
    if volume.files.len() + package.keys().filter(|p| !volume.files.contains_key(*p)).count()
        > disk::ENTRY_LIMIT {
        return Err("filesystem full");
    }
    let mut undo = Vec::with_capacity(package.len());
    for (path, value) in package {
        let old = volume.files.insert(path.clone(), value);
        undo.push((path, old));
    }
    if let Err(error) = volume.save_changes(
        undo.iter().map(|(path, old)| (path.as_str(), old.is_some())),
        &disk::Times::new(),
    ) {
        for (path, old) in undo {
            match old {
                Some(value) => { volume.files.insert(path, value); }
                None => { volume.files.remove(&path); }
            }
        }
        return Err(error);
    }
    Ok(())
}
