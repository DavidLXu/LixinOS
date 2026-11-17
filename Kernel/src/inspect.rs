//! Read-only observations from the guest, independent of the model bridge.
use crate::{
    HEAP_BYTES, disk,
    monitor::{heap_usage, kernel_span_bytes, seconds_to_ticks, ticks},
    user,
};
use alloc::{format, string::String};
use core::{fmt::Write, ptr::read_volatile, slice};

type Result<T> = core::result::Result<T, &'static str>;

fn device_tree() -> Option<fdt::Fdt<'static>> {
    // QEMU virt places the bare-metal ELF's DTB at RAM start. The linker
    // leaves the first 2 MiB below the kernel image available for it.
    const BASE: usize = 0x4000_0000;
    const MAX_SIZE: usize = 0x20_0000;
    let magic = u32::from_be(unsafe { read_volatile(BASE as *const u32) });
    if magic != 0xd00d_feed {
        return None;
    }
    let size = u32::from_be(unsafe { read_volatile((BASE + 4) as *const u32) }) as usize;
    if !(40..=MAX_SIZE).contains(&size) {
        return None;
    }
    fdt::Fdt::new(unsafe { slice::from_raw_parts(BASE as *const u8, size) }).ok()
}

pub fn cpu(started: u64) -> String {
    let vcpus = device_tree()
        .and_then(|tree| tree.find_node("/cpus").map(|_| tree.cpus().count()))
        .filter(|&count| count != 0);
    let frequency = seconds_to_ticks(1);
    let uptime = ticks().wrapping_sub(started) / frequency;
    #[cfg(feature = "retina")]
    let busy = crate::graphics::cpu_busy_percent();
    #[cfg(not(feature = "retina"))]
    let busy: Option<u64> = None;
    format!(
        "architecture=aarch64\n         vcpus={} (QEMU boot device tree)\n         uptime_seconds={uptime}\n         architectural_timer_hz={frequency} (not CPU clock speed)\n         guest_busy_percent={} (latest console sample, excludes input waits; not Mac CPU utilization)",
        measurement(vcpus),
        measurement(busy)
    )
}

pub fn memory() -> String {
    let ram = device_tree().and_then(|tree| {
        tree.find_node("/memory")?;
        let total = tree
            .memory()
            .regions()
            .try_fold(0usize, |sum, region| sum.checked_add(region.size?))?;
        (total != 0).then_some(total)
    });
    let (used, free) = heap_usage();
    let kernel = kernel_span_bytes() - HEAP_BYTES;
    let reserved = user::BANKS * user::USER_REGION_BYTES;
    format!(
        "guest_ram_bytes={} (QEMU boot device tree)\n         heap_used_bytes={used}\nheap_free_bytes={free}\n         kernel_image_and_stacks_excluding_heap_arena_bytes={kernel}\n         user_program_ram_reserved_bytes={reserved}\n         known_usage_estimate_bytes={} (kernel span excluding heap arena, plus allocated heap and reserved user regions)\n         total_guest_free_bytes=unavailable (no physical-page allocator)",
        measurement(ram),
        kernel + used + reserved
    )
}

fn measurement(value: Option<impl core::fmt::Display>) -> String {
    value.map_or_else(|| "unavailable".into(), |value| format!("{value}"))
}

pub fn disk(disk: &disk::Disk) -> String {
    let (sectors, generation) = disk.snapshot_status();
    let capacity = sectors as u128 * 512;
    format!(
        "device=virtio-blk\ncapacity_bytes={capacity}\nsector_count={sectors}\nsector_bytes=512\n         filesystem_mounted=true\ncommitted_snapshot_generation={generation}\n         persistence=two alternating checksummed snapshots on the guest disk; the newest valid snapshot is loaded at boot"
    )
}

pub fn filesystem(files: &disk::Files) -> String {
    let (count, dirs, bytes) = disk::file_counts(files);
    format!(
        "files={count}\ndirectories={dirs} (includes root)\nfile_content_bytes={bytes}\n         entries={}\nentry_limit={}\nfile_size_limit_bytes={}\n         file_content_bytes_excludes=filesystem metadata, history and snapshot overhead",
        files.len(), disk::ENTRY_LIMIT, disk::FILE_LIMIT
    )
}

pub fn stat(files: &disk::Files, path: &str) -> Result<String> {
    match files.get(path).ok_or("path not found")? {
        Some(bytes) => Ok(format!("path={path:?}\ntype=file\nsize_bytes={}", bytes.len())),
        None => Ok(format!(
            "path={path:?}\ntype=directory\ndirect_children={}",
            disk::children(files, path).count()
        )),
    }
}

pub fn list(files: &disk::Files, path: &str) -> Result<String> {
    const LIST_LIMIT: usize = 4096;
    if files.get(path).ok_or("path not found")?.is_some() {
        return Err("not a directory");
    }
    let mut output = format!("path={path:?}\n");
    let mut shown = 0;
    let total = disk::children(files, path).count();
    for (name, value) in disk::children(files, path) {
        let row = match value {
            Some(bytes) => format!("file {name:?} ({} bytes)\n", bytes.len()),
            None => format!("directory {name:?}\n"),
        };
        if output.len() + row.len() > LIST_LIMIT {
            break;
        }
        output.push_str(&row);
        shown += 1;
    }
    write!(output, "entries_shown={shown}\ndirect_children={total}").unwrap();
    if shown < total {
        output.push_str("\nlisting_truncated=true");
    }
    Ok(output)
}
