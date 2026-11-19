//! Read-only, Linux-shaped proc files backed by LixinOS counters.
use crate::{inspect, monitor, scheduler};
use alloc::{format, string::String};

pub(crate) const ROOT: &str = "/proc";

#[derive(Clone, Copy)]
pub(crate) enum File {
    CpuInfo,
    MemInfo,
    Stat,
    Uptime,
    Version,
}

// Keep directory entries in lexical order.
pub(crate) const FILES: &[(&str, File)] = &[
    ("cpuinfo", File::CpuInfo),
    ("meminfo", File::MemInfo),
    ("stat", File::Stat),
    ("uptime", File::Uptime),
    ("version", File::Version),
];

pub(crate) fn contains(path: &str) -> bool {
    path == ROOT || path.strip_prefix(ROOT).is_some_and(|suffix| suffix.starts_with('/'))
}

pub(crate) fn file(path: &str) -> Option<File> {
    let name = path.strip_prefix("/proc/")?;
    FILES.iter().find_map(|&(key, file)| (key == name).then_some(file))
}

fn cpu_times() -> (u64, u64) {
    let uptime = scheduler::uptime_ticks();
    let idle = monitor::cpu_counters().idle_ticks.min(uptime);
    let frequency = u128::from(monitor::seconds_to_ticks(1));
    let centiseconds = |ticks| (u128::from(ticks) * 100 / frequency) as u64;
    (centiseconds(uptime), centiseconds(idle))
}

impl File {
    pub(crate) fn read(self) -> String {
        match self {
            Self::CpuInfo => format!(
                "processor\t: 0\nmodel name\t: {} {} vCPU\n",
                inspect::SYSTEM_NAME, inspect::ARCHITECTURE,
            ),
            Self::MemInfo => {
                let (_, free) = monitor::heap_usage();
                let total = inspect::ram_size().map_or_else(String::new, |bytes| {
                    format!("MemTotal:       {} kB\n", bytes / 1024)
                });
                let free = free / 1024;
                format!(
                    "{total}MemFree:        {free} kB\nMemAvailable:   {free} kB\n\
                     SwapTotal:      0 kB\nSwapFree:       0 kB\n"
                )
            }
            Self::Stat => {
                let (uptime, idle) = cpu_times();
                // Accounting separates scheduler busy time from idle waits.
                // Report total busy time in the system column for delta readers.
                let busy = uptime - idle;
                format!("cpu  0 0 {busy} {idle} 0 0 0 0 0 0\ncpu0 0 0 {busy} {idle} 0 0 0 0 0 0\n")
            }
            Self::Uptime => {
                let (uptime, idle) = cpu_times();
                format!("{}.{:02} {}.{:02}\n", uptime / 100, uptime % 100, idle / 100, idle % 100)
            }
            Self::Version => format!("{}\n", inspect::system_summary()),
        }
    }
}
