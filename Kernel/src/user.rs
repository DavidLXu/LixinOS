// Each terminal maps its private EL0 image at the same virtual addresses.
// The kernel-only alias permits loading RX text.
use alloc::vec::Vec;
use core::arch::{asm, global_asm};
use core::cell::UnsafeCell;
use core::ptr::{addr_of_mut, copy_nonoverlapping, write_bytes};

global_asm!(include_str!("user.S"), split=const cfg!(feature="split") as usize);
unsafe extern "C" {
    fn enter_user(path: usize, source: usize, len: usize, save: usize) -> i64;
    fn user_vectors();
}
#[repr(align(4096))]
struct Table([u64; 512]);
pub const BANKS: usize = if cfg!(feature = "split") { 2 } else { 1 };
static mut ROOT: [Table; BANKS] = [const { Table([0; 512]) }; BANKS];
static mut RAM: [Table; BANKS] = [const { Table([0; 512]) }; BANKS];
const USER: usize = 0x44000000;
const ALIAS: usize = 0x50000000;
const BLOCK: usize = 2 << 20;
pub const USER_REGION_BYTES: usize = 2 * BLOCK;
const AF: u64 = 1 << 10;
const SH: u64 = 3 << 8;
const UXN: u64 = 1 << 54;
const PXN: u64 = 1 << 53;
const USER_ACCESS: u64 = 1 << 6;
const READ_ONLY: u64 = 1 << 7;

pub const PYTHON: &[u8] = include_bytes!("../target/user-build/python.bin");
pub const TEST_OUTPUT_LIMIT: usize = 2048;

struct TestIo {
    input: Vec<u8>,
    position: usize,
    output: Vec<u8>,
    deadline: u64,
    stopped: Option<&'static str>,
}

// The foreground EL0 slot is single-core and uses synchronous syscalls.
struct TestSlot(UnsafeCell<[Option<TestIo>; BANKS]>);
unsafe impl Sync for TestSlot {}
static TEST_IO: TestSlot = TestSlot(UnsafeCell::new([const { None }; BANKS]));

fn bank() -> usize {
    #[cfg(feature = "split")]
    {
        crate::panes::active()
    }
    #[cfg(not(feature = "split"))]
    {
        0
    }
}
fn test_clock() -> u64 {
    #[cfg(feature = "split")]
    {
        crate::panes::ticks()
    }
    #[cfg(not(feature = "split"))]
    {
        crate::monitor::ticks()
    }
}

pub fn test_console(value: u64, operation: u64) -> Option<u64> {
    let io = unsafe { (&mut *TEST_IO.0.get())[bank()].as_mut()? };
    let stopped = if test_clock() >= io.deadline {
        Some("test timed out")
    } else if operation == 1 && io.output.len() == TEST_OUTPUT_LIMIT {
        Some("test exceeded the output limit")
    } else {
        None
    };
    if let Some(reason) = stopped {
        io.stopped = Some(reason);
        // The exception handler unwinds to the shell for this return value.
        return Some((-2i64) as u64);
    }
    Some(match operation {
        0 => match io.input.get(io.position) {
            Some(&byte) => {
                io.position += 1;
                u64::from(byte)
            }
            None => 256, // EOF outside the byte range.
        },
        1 => {
            io.output.push(value as u8);
            value & 255
        }
        3 => 0,
        5 => 1,
        _ => u64::MAX,
    })
}

pub fn test_python(path: &str, source: &str, input: &str) -> (Result<(), &'static str>, Vec<u8>) {
    let io = TestIo {
        input: input.as_bytes().to_vec(),
        position: 0,
        output: Vec::with_capacity(TEST_OUTPUT_LIMIT),
        deadline: test_clock() + crate::monitor::seconds_to_ticks(2),
        stopped: None,
    };
    unsafe { (*TEST_IO.0.get())[bank()] = Some(io) };
    let result = run(PYTHON, Some((path, source)));
    let io = unsafe { (&mut *TEST_IO.0.get())[bank()].take().unwrap() };
    (io.stopped.map_or(result, Err), io.output)
}

fn setup_bank(bank: usize) {
    unsafe {
        // MAIR slot 0 = Normal non-cacheable, slot 1 = Device-nGnRnE.
        ROOT[bank].0[0] = 1 | AF | 4 | PXN | UXN;
        ROOT[bank].0[1] = addr_of_mut!(RAM[bank]) as u64 | 3;
        for i in 0..64 {
            let pa = 0x40000000 + i * BLOCK;
            let permissions = match pa {
                USER => USER_ACCESS | READ_ONLY | PXN,
                0x44200000 => USER_ACCESS | PXN | UXN,
                _ => UXN,
            };
            let backing = if (USER..USER + USER_REGION_BYTES).contains(&pa) {
                pa + bank * USER_REGION_BYTES
            } else {
                pa
            };
            RAM[bank].0[i] = backing as u64 | AF | SH | permissions | 1;
        }
        for i in 0..2 {
            RAM[bank].0[(ALIAS - 0x40000000) / BLOCK + i] =
                (USER + bank * USER_REGION_BYTES + i * BLOCK) as u64 | AF | SH | PXN | UXN | 1;
        }
    }
}

#[cfg(feature = "split")]
pub fn select_bank(bank: usize) {
    unsafe {
        asm!("dsb ish", "msr ttbr0_el1, {}", "isb", "tlbi vmalle1", "dsb ish", "ic iallu", "dsb ish", "isb",
            in(reg) addr_of_mut!(ROOT[bank]));
    }
}

pub fn init() {
    for bank in 0..BANKS {
        setup_bank(bank);
    }
    unsafe {
        asm!("msr vbar_el1, {vectors}", "msr mair_el1, {attr}", "msr tcr_el1, {tcr}",
            "msr ttbr0_el1, {table}", "dsb sy","isb","tlbi vmalle1","dsb sy","isb",
            "mrs x9, sctlr_el1","orr x9,x9,#1","msr sctlr_el1,x9","isb",
            "mrs x9,cpacr_el1","orr x9,x9,#(3<<20)","msr cpacr_el1,x9","isb",
            vectors=in(reg)user_vectors as *const () as usize,attr=in(reg)0x44u64,
            tcr=in(reg)(25u64|(1<<23)|(3<<12)),table=in(reg)addr_of_mut!(ROOT),out("x9") _);
    }
}
// Reserved by User/user.ld immediately below the user stack.
const SCRIPT_OFFSET: usize = 0x3d8000;
const SCRIPT_CAPACITY: usize = 32 * 1024;

// Publish only into the reserved EL0 RX area, through its EL1-only alias.
pub fn publish_code(src: usize) -> u64 {
    const SIZE: usize = 256 * 1024;
    if !(USER + BLOCK..=USER + USER_REGION_BYTES - SIZE).contains(&src) {
        return u64::MAX;
    }
    unsafe {
        copy_nonoverlapping(
            (ALIAS + (src - USER)) as *const u8,
            (ALIAS + 0x180000) as *mut u8,
            SIZE,
        );
        asm!("dsb sy", "ic iallu", "dsb sy", "isb");
    }
    0
}
pub fn run(image: &[u8], script: Option<(&str, &str)>) -> Result<(), &'static str> {
    launch(image, script, false)
}

pub const TCC: &[u8] = include_bytes!("../target/user-build/tcc.bin");
const RUNNER: &[u8] = include_bytes!("../target/user-build/runner.bin");

struct Executable<'a> {
    entry: usize,
    code: &'a [u8],
    data: &'a [u8],
}

fn executable(bytes: &[u8]) -> Result<Executable<'_>, &'static str> {
    if bytes.len() < 16 || &bytes[..4] != b"LXE1" {
        return Err("invalid LixinOS executable");
    }
    let field = |i: usize| u32::from_le_bytes(bytes[i..i + 4].try_into().unwrap()) as usize;
    let (entry, code, data) = (field(4), field(8), field(12));
    let end = 16 + code + data;
    if end > bytes.len() || end > 16384 || entry % 4 != 0 || entry + 4 > code {
        return Err("invalid executable layout");
    }
    Ok(Executable {
        entry,
        code: &bytes[16..16 + code],
        data: &bytes[16 + code..end],
    })
}

pub fn compile(path: &str, source: &str) -> Result<Vec<u8>, &'static str> {
    launch(TCC, Some((path, source)), true)?;
    // The EL0 call has returned and the foreground slot is exclusively ours.
    let result = unsafe {
        core::slice::from_raw_parts((ALIAS + SCRIPT_OFFSET) as *const u8, SCRIPT_CAPACITY)
    };
    let image = executable(result)?;
    Ok(result[..16 + image.code.len() + image.data.len()].to_vec())
}

pub fn run_saved(bytes: &[u8]) -> Result<(), &'static str> {
    let image = executable(bytes)?;
    if 16 + image.code.len() + image.data.len() != bytes.len() {
        return Err("unexpected executable length");
    }
    unsafe {
        write_bytes(ALIAS as *mut u8, 0, USER_REGION_BYTES);
        copy_nonoverlapping(RUNNER.as_ptr(), ALIAS as *mut u8, RUNNER.len());
        copy_nonoverlapping(
            image.code.as_ptr(),
            (ALIAS + 0x180000) as *mut u8,
            image.code.len(),
        );
        copy_nonoverlapping(
            image.data.as_ptr(),
            (ALIAS + 0x300000) as *mut u8,
            image.data.len(),
        );
        asm!("dsb sy", "ic iallu", "dsb sy", "isb");
        if enter_user(USER + 0x180000 + image.entry, 0, 0, 0) != 0 {
            return Err("executable fault");
        }
    }
    Ok(())
}

fn launch(image: &[u8], script: Option<(&str, &str)>, save: bool) -> Result<(), &'static str> {
    if image.len() > SCRIPT_OFFSET {
        return Err("user image too large");
    }
    unsafe {
        write_bytes(ALIAS as *mut u8, 0, USER_REGION_BYTES);
        copy_nonoverlapping(image.as_ptr(), ALIAS as *mut u8, image.len());
        let (path_ptr, source_ptr, len) = if let Some((path, source)) = script {
            let offset = path.len() + 1; // zeroed terminator
            if offset + source.len() > SCRIPT_CAPACITY {
                return Err("script exceeds launch buffer");
            }
            let dest = (ALIAS + SCRIPT_OFFSET) as *mut u8;
            copy_nonoverlapping(path.as_ptr(), dest, path.len());
            copy_nonoverlapping(source.as_ptr(), dest.add(offset), source.len());
            let base = USER + SCRIPT_OFFSET;
            (base, base + offset, source.len())
        } else {
            (0, 0, 0)
        };
        asm!("dsb sy", "ic iallu", "dsb sy", "isb");
        match enter_user(path_ptr, source_ptr, len, usize::from(save)) {
            0 => (),
            -1 => return Err("user program fault"),
            _ => return Err("user program exited unsuccessfully"),
        }
    }
    Ok(())
}
#[unsafe(no_mangle)]
extern "C" fn kernel_vector_fault() -> ! {
    crate::print("Fatal EL1 exception\n");
    crate::idle()
}
