//! Read-only observations from the guest, independent of the model bridge.
use crate::{
    disk,
    monitor::{self, heap_usage, kernel_span_bytes, seconds_to_ticks},
    process::{Entry, Volume},
};
use alloc::{format, string::String};
use core::{fmt::Write, ptr::read_volatile, slice};

type Result<T> = core::result::Result<T, &'static str>;

pub const SYSTEM_NAME: &str = "LixinOS";
pub const SYSTEM_RELEASE: &str = concat!(env!("LIXINOS_KERNEL_TAG"), "-rust");
pub const ARCHITECTURE: &str = "aarch64";
pub fn system_summary() -> String {
    format!("{SYSTEM_NAME} {SYSTEM_RELEASE} {ARCHITECTURE} (standalone; non-Linux)")
}

pub fn system() -> String {
    format!(
        "name={SYSTEM_NAME}\nrelease={SYSTEM_RELEASE}\narchitecture={ARCHITECTURE}\nlinux_kernel=false\nplatform=QEMU virt guest\nidentity_command=uname -a"
    )
}

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

pub fn cpu() -> String {
    let vcpus = device_tree()
        .and_then(|tree| tree.find_node("/cpus").map(|_| tree.cpus().count()))
        .filter(|&count| count != 0);
    let frequency = seconds_to_ticks(1);
    let start = monitor::cpu_counters();
    crate::scheduler::sleep_ms(250);
    let end = monitor::cpu_counters();
    let busy = end.busy_percent(start);
    let sample_ms = end.ticks.wrapping_sub(start.ticks) * 1000 / frequency;
    let uptime = crate::scheduler::uptime_ticks() / frequency;
    format!(
        "architecture={ARCHITECTURE}\nvcpus={} (QEMU boot device tree)\n\
         uptime_seconds={uptime}\narchitectural_timer_hz={frequency}\n\
         guest_busy_percent={}\nsample_ms={sample_ms}\nscope=guest_scheduler",
        measurement(vcpus),
        measurement(busy)
    )
}

pub(crate) fn ram_size() -> Option<usize> {
     device_tree().and_then(|tree| {
        tree.find_node("/memory")?;
        let total = tree
            .memory()
            .regions()
            .try_fold(0usize, |sum, region| sum.checked_add(region.size?))?;
        (total != 0).then_some(total)
    })
}

pub fn memory() -> String {
    let ram = ram_size();
    let (used, free) = heap_usage();
    let kernel = kernel_span_bytes();
    let timer_irqs=crate::scheduler::timer_interrupts();
    format!(
        "guest_ram_bytes={} (QEMU boot device tree)\n         heap_used_bytes={used}\nheap_free_bytes={free}\n         kernel_image_and_stacks_excluding_heap_arena_bytes={kernel}\n         user_program_ram_reserved_bytes=0 (private images charged to heap)\n         scheduler_timer_interrupts={timer_irqs}\n         known_usage_estimate_bytes={} (kernel span excluding heap arena, plus allocated heap)\n         total_guest_free_bytes=unavailable (no physical-page allocator)",
        measurement(ram),
        kernel + used
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

pub fn stat(volume: &Volume, path: &str) -> Result<String> {
    let entry = volume.entry(path).ok_or("path not found")?;
    match entry.size() {
        Some(size) => Ok(format!("path={path:?}\ntype=file\nsize_bytes={size}")),
        None => Ok(format!(
            "path={path:?}\ntype=directory\ndirect_children={}",
            volume.children(path, "", None).count()
        )),
    }
}

pub fn list(volume: &Volume, path: &str) -> Result<String> {
    const LIST_LIMIT: usize = 4096;
    if !matches!(volume.entry(path).ok_or("path not found")?, Entry::Directory) {
        return Err("not a directory");
    }
    let mut output = format!("path={path:?}\n");
    let mut shown = 0;
    let total = volume.children(path, "", None).count();
    for (name, entry) in volume.children(path, "", None) {
        let row = match entry.size() {
            Some(size) => format!("file {name:?} ({size} bytes)\n"),
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
