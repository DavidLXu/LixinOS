// Each scheduled process maps its private EL0 image at the same virtual addresses.
// The kernel-only alias permits loading RX text.
use alloc::vec::Vec;
use core::arch::{asm, global_asm};
use core::cell::UnsafeCell;
use core::ptr::{addr_of_mut, copy_nonoverlapping, write_bytes};

global_asm!(include_str!("user.S"));
unsafe extern "C" {
    fn enter_user(path: usize, source: usize, len: usize, save: usize) -> i64;
    fn user_vectors();
}
#[repr(align(4096))]
struct Table([u64; 512]);
pub const BANKS: usize = crate::scheduler::CAPACITY;
#[repr(C)]
struct Tables {
    root: Table,
    ram: Table,
    extended: Table,
    program: Table,
    shared: Table,
    wide: [Table; 4],
}
static mut SPACES: [*mut Tables; BANKS] = [core::ptr::null_mut(); BANKS];
fn tables(slot: usize) -> *mut Tables {
    unsafe { SPACES[slot] }
}
pub fn free_space(slot: usize) {
    debug_assert!(slot != bank());
    unsafe {
        let table = core::mem::replace(&mut (*addr_of_mut!(SPACES))[slot], core::ptr::null_mut());
        if !table.is_null() {
            drop(alloc::boxed::Box::from_raw(table));
        }
    }
}

const USER: usize = 0x44000000;
const ALIAS: usize = 0x70000000;
const BLOCK: usize = 2 << 20;
pub const USER_REGION_BYTES: usize = 2 * BLOCK;
const AF: u64 = 1 << 10;
const SH: u64 = 3 << 8;
const UXN: u64 = 1 << 54;
const PXN: u64 = 1 << 53;
const USER_ACCESS: u64 = 1 << 6;
const READ_ONLY: u64 = 1 << 7;

pub static PYTHON: &[u8] = include_bytes!("../target/user-build/python.bin");
pub const TEST_OUTPUT_LIMIT: usize = 2048;

struct TestIo {
    input: Vec<u8>,
    position: usize,
    output: Vec<u8>,
    deadline: u64,
    stopped: Option<&'static str>,
    interrupt: fn() -> Option<&'static str>,
}

// Test I/O is only accessed by synchronous syscalls on the primary CPU.
struct TestSlot(UnsafeCell<[Option<TestIo>; BANKS]>);
unsafe impl Sync for TestSlot {}
static TEST_IO: TestSlot = TestSlot(UnsafeCell::new([const { None }; BANKS]));

// The utility ABI reserves the preceding page for a metadata request and result.
pub(crate) fn utility_mailbox() -> *mut u8 {
    (ALIAS + UTILITY_OFFSET) as *mut u8
}
pub(crate) fn git_mailbox() -> *mut u8 {
    (ALIAS + SCRIPT_OFFSET) as *mut u8
}
pub(crate) fn bank() -> usize {
    crate::scheduler::active()
}
pub(crate) fn test_io_active() -> bool {
    unsafe { (&*TEST_IO.0.get())[bank()].is_some() }
}
fn test_clock() -> u64 {
    crate::scheduler::ticks()
}

pub fn test_console(value: u64, operation: u64) -> Option<u64> {
    let io = unsafe { (&mut *TEST_IO.0.get())[bank()].as_mut()? };
    let stopped = if matches!(operation, 0 | 3 | 41 | 43) { (io.interrupt)() } else { None };
    let stopped = if stopped.is_some() {
        stopped
    } else if test_clock() >= io.deadline {
        Some("test timed out")
    } else if matches!(operation, 1 | 11) && io.output.len() == TEST_OUTPUT_LIMIT {
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
        1 | 11 => {
            io.output.push(value as u8);
            value & 255
        }
        3 => 0,
        5 => 1,
        38 if value == 0 => {
            let ticks = crate::monitor::ticks();
            let frequency = crate::monitor::seconds_to_ticks(1);
            ticks / frequency * 1000 + ticks % frequency * 1000 / frequency
        }
        // EOF is readable, just like EOF on a redirected stdin.
        41 if value == 1 << 32 => 1,
        41 if value == (1 << 48 | 2 << 32) || value == (2 << 48 | 2 << 32) => 2,
        43 if value <= 1 => { crate::scheduler::sleep_ms(value); 0 }
        _ => u64::MAX,
    })
}

pub fn test_python(path: &str, source: &str, input: &str) -> (Result<(), &'static str>, Vec<u8>) {
    test_python_interruptible(path, source, input, || None)
}

pub(crate) fn test_python_interruptible(
    path: &str, source: &str, input: &str, interrupt: fn() -> Option<&'static str>,
) -> (Result<(), &'static str>, Vec<u8>) {
    let io = TestIo {
        input: input.as_bytes().to_vec(),
        position: 0,
        output: Vec::with_capacity(TEST_OUTPUT_LIMIT),
        deadline: test_clock() + crate::monitor::seconds_to_ticks(2),
        stopped: None,
        interrupt,
    };
    unsafe { (*TEST_IO.0.get())[bank()] = Some(io) };
    let result = run(PYTHON, Some((path, source)));
    let io = unsafe { (&mut *TEST_IO.0.get())[bank()].take().unwrap() };
    (io.stopped.map_or(result, Err), io.output)
}

pub fn setup_bank(bank: usize) -> Result<(), &'static str> {
    let ptr = unsafe { alloc::alloc::alloc_zeroed(core::alloc::Layout::new::<Tables>()) }
        .cast::<Tables>();
    if ptr.is_null() {
        return Err("out of memory for page tables");
    }
    unsafe {
        SPACES[bank] = ptr;
    }
    unsafe {
        // MAIR slot 0 = Normal write-through RAM (also permits EL0 exclusives), slot 1 = Device-nGnRnE.
        (*tables(bank)).root.0[0] = 1 | AF | 4 | PXN | UXN;
        (*tables(bank)).root.0[1] = addr_of_mut!((*tables(bank)).ram) as u64 | 3;
        (*tables(bank)).root.0[2] = addr_of_mut!((*tables(bank)).program) as u64 | 3;
        (*tables(bank)).root.0[3] = addr_of_mut!((*tables(bank)).extended) as u64 | 3;
        (*tables(bank)).root.0[SHARED_BASE / crate::GIB] = addr_of_mut!((*tables(bank)).shared) as u64 | 3;
        for i in 0..crate::MAPPED_RAM / BLOCK {
            let pa = 0x40000000 + i * BLOCK;
            (*tables(bank)).ram.0[i] = pa as u64 | AF | SH | UXN | 1;
        }
        if let Some(end) = crate::high_heap_end() {
            for i in 0..4 {
                (*tables(bank)).root.0[WIDE_BASE / crate::GIB + i] = addr_of_mut!((*tables(bank)).wide[i]) as u64 | 3;
            }
            for address in (crate::HIGH_HEAP..end).step_by(crate::GIB) {
                (*tables(bank)).root.0[address / crate::GIB] = address as u64 | AF | SH | PXN | UXN | 1;
            }
        }
        // No program has been launched into this address space yet.
        for i in 0..2 {
            (*tables(bank)).ram.0[(USER - 0x40000000) / BLOCK + i] = 0;
        }
    }
    Ok(())
}

pub fn select_bank(bank: usize) {
    unsafe {
        asm!("dsb ish", "msr ttbr0_el1, {}", "isb", "tlbi vmalle1", "dsb ish", "ic iallu", "dsb ish", "isb",
            in(reg) addr_of_mut!((*tables(bank)).root));
    }
}

// A process owns the physical image behind its fixed EL0 virtual addresses.
// A suspended parent retains these blocks. Running a child never overwrites them.
pub(crate) struct ImageMemory(core::ptr::NonNull<u8>);
impl ImageMemory {
    fn layout() -> core::alloc::Layout {
        core::alloc::Layout::from_size_align(USER_REGION_BYTES, BLOCK).unwrap()
    }
    fn new() -> Result<Self, &'static str> {
        core::ptr::NonNull::new(unsafe { alloc::alloc::alloc(Self::layout()) })
            .map(Self)
            .ok_or("not enough memory for process image")
    }
}
impl Drop for ImageMemory {
    fn drop(&mut self) {
        unsafe {
            alloc::alloc::dealloc(self.0.as_ptr(), Self::layout());
        }
    }
}
static mut IMAGES: [Option<ImageMemory>; BANKS] = [const { None }; BANKS];

static mut ARENA_BYTES: [usize; BANKS] = [0; BANKS];
pub fn private_bytes(slot: usize) -> usize {
    unsafe { ARENA_BYTES[slot] + if (*addr_of_mut!(IMAGES))[slot].is_some() {USER_REGION_BYTES} else {0} }
}

fn replace_image(image: Option<ImageMemory>) -> Option<ImageMemory> {
    let slot = bank();
    let base = image.as_ref().map(|mem| mem.0.as_ptr() as usize);
    unsafe {
        for i in 0..2 {
            (*tables(slot)).ram.0[(USER - 0x40000000) / BLOCK + i] = base.map_or(0, |p| {
                let perms = if i == 0 { READ_ONLY } else { UXN };
                (p + i * BLOCK) as u64 | 1 | AF | SH | USER_ACCESS | PXN | perms
            });
            (*tables(slot)).ram.0[(ALIAS - 0x40000000) / BLOCK + i] =
                base.map_or(0, |p| (p + i * BLOCK) as u64 | 1 | AF | SH | PXN | UXN);
        }
        asm!(
            "dsb ishst",
            "tlbi vmalle1",
            "dsb ish",
            "ic iallu",
            "dsb ish",
            "isb"
        );
        core::mem::replace(&mut (*addr_of_mut!(IMAGES))[slot], image)
    }
}
pub fn release_image() {
    replace_image(None);
}
fn ensure_image() -> Result<(), &'static str> {
    if unsafe { (*addr_of_mut!(IMAGES))[bank()].is_none() } {
        replace_image(Some(ImageMemory::new()?));
    }
    Ok(())
}

pub fn init() {
    setup_bank(0).expect("boot address space");
    unsafe {
        asm!("msr vbar_el1, {vectors}", "msr mair_el1, {attr}", "msr tcr_el1, {tcr}",
            "msr ttbr0_el1, {table}", "dsb sy","isb","tlbi vmalle1","dsb sy","isb",
            "mrs x9, sctlr_el1","orr x9,x9,#1","orr x9,x9,#4","orr x9,x9,#0x1000","msr sctlr_el1,x9","isb",
            "mrs x9,cpacr_el1","orr x9,x9,#(3<<20)","msr cpacr_el1,x9","isb",
            vectors=in(reg)user_vectors as *const () as usize,attr=in(reg)0xffu64,
            tcr=in(reg)(25u64|(1<<23)|(3<<12)|((crate::high_heap_end().is_some() as u64)<<32)),table=in(reg)addr_of_mut!((*tables(0)).root),out("x9") _);
    }
}
// Reserved by User/user.ld immediately below the user stack.
const SCRIPT_OFFSET: usize = 0x3d8000;
pub(crate) const SCRIPT_CAPACITY: usize = 32 * 1024;

static mut CAN_PUBLISH: [bool; BANKS] = [false; BANKS];

// Publish only into the reserved EL0 RX area, through its EL1-only alias.
pub fn publish_code(src: usize) -> u64 {
    if !unsafe { CAN_PUBLISH[bank()] } {
        return u64::MAX;
    }
    const SIZE: usize = 256 * 1024;
    if !(USER + BLOCK..=USER + USER_REGION_BYTES - SIZE).contains(&src) {
        return u64::MAX;
    }
    unsafe {
        CAN_PUBLISH[bank()] = false;
        copy_nonoverlapping(
            (ALIAS + (src - USER)) as *const u8,
            (ALIAS + LXE_CODE_OFFSET) as *mut u8,
            SIZE,
        );
        asm!("dsb sy", "ic iallu", "dsb sy", "isb");
    }
    0
}
pub fn run(image: &[u8], script: Option<(&str, &str)>) -> Result<(), &'static str> {
    match launch(image, script, false)? {
        0 => Ok(()),
        _ => Err("user program exited unsuccessfully"),
    }
}

pub static TCC: &[u8] = include_bytes!("../target/user-build/tcc.bin");
const RUNNER: &[u8] = include_bytes!("../target/user-build/runner.bin");

const LXE_CODE_OFFSET: usize = 0x180000;
const LXE_DATA_OFFSET: usize = 0x300000;
const UTILITY_OFFSET: usize = SCRIPT_OFFSET - 4096;

// SVC 22 and the built-in compiler can return sparse file contents without
// materializing a second contiguous copy in the EL0 heap. Descriptor layout:
// (table address << 32) | file length. Table: count, then (offset, addr, len)
// triples of little-endian u32. Unspecified bytes, including BSS, are zeroes.
pub(crate) fn copy_user_file(descriptor: u64, heap: Option<&UserArena>) -> Option<Vec<u8>> {
    let size = descriptor as u32 as usize;
    if size > crate::disk::FILE_LIMIT { return None; }
    // Accept initialized parts from the active task's image or its mapped heap.
    let bank = unsafe { core::slice::from_raw_parts((ALIAS + BLOCK) as *const u8, UTILITY_OFFSET - BLOCK) };
    let buffer = |address: usize, length: usize| {
        if heap.is_some_and(|h| h.contains(address, length)) {
            return Some(unsafe { core::slice::from_raw_parts(address as *const u8, length) });
        }
        let start = address.checked_sub(USER + BLOCK)?;
        bank.get(start..start.checked_add(length)?)
    };
    let word = |bytes: &[u8]| u32::from_le_bytes(bytes.try_into().unwrap()) as usize;
    let address = (descriptor >> 32) as usize;
    let count = word(buffer(address, 4)?);
    // Nonempty parts contribute at least one byte apiece.
    if count > size { return None; }
    let table = buffer(address.checked_add(4)?, count.checked_mul(12)?)?;
    let mut file = Vec::new();
    file.try_reserve_exact(size).ok()?;
    file.resize(size, 0);
    let mut remaining = size;
    for part in table.chunks_exact(12) {
        let offset = word(&part[..4]);
        let len = word(&part[8..]);
        if len == 0 { return None; }
        remaining = remaining.checked_sub(len)?;
        let src = buffer(word(&part[4..8]), len)?;
        file.get_mut(offset..offset.checked_add(len)?)?.copy_from_slice(src);
    }
    Some(file)
}

struct Executable<'a> {
    entry: usize,
    code: &'a [u8],
    data: &'a [u8],
}

fn executable(bytes: &[u8]) -> Result<Executable<'_>, &'static str> {
    const HEADER: usize = 16;
    if bytes.len() < HEADER || !bytes.starts_with(b"LXE1") {
        return Err("invalid LixinOS executable");
    }
    let field = |i: usize| u32::from_le_bytes(bytes[i..i + 4].try_into().unwrap()) as usize;
    let (entry, code, data) = (field(4), field(8), field(12));
    let end = HEADER + code + data;
    if end != bytes.len() || end > crate::disk::FILE_LIMIT
        || code > BLOCK - LXE_CODE_OFFSET || data > UTILITY_OFFSET - LXE_DATA_OFFSET
        || entry % 4 != 0 || entry + 4 > code {
        return Err("invalid executable layout");
    }
    Ok(Executable {
        entry,
        code: &bytes[HEADER..HEADER + code],
        data: &bytes[HEADER + code..end],
    })
}

pub fn is_executable(bytes: &[u8]) -> bool {
    executable(bytes).is_ok()
}

pub fn compile(path: &str, source: &str) -> Result<Vec<u8>, &'static str> {
    if launch(TCC, Some((path, source)), true)? != 0 { return Err("compilation failed"); }
    // The EL0 call has returned and the foreground slot is exclusively ours.
    let descriptor = unsafe { git_mailbox().cast::<u64>().read() };
    let bytes = crate::utility::compiler_file(descriptor).ok_or("invalid compiler result buffer")?;
    if !is_executable(&bytes) { return Err("invalid compiler output"); }
    Ok(bytes)
}

pub(crate) fn start_saved(bytes: &[u8], argc: usize, packed: &str, flags: usize) -> Result<u16, &'static str> {
    let image = executable(bytes)?;
    if packed.len() > SCRIPT_CAPACITY || argc > 512 { return Err("arguments too large"); }
    ensure_image()?;
    unsafe {
        write_bytes(ALIAS as *mut u8, 0, USER_REGION_BYTES);
        copy_nonoverlapping(RUNNER.as_ptr(), ALIAS as *mut u8, RUNNER.len());
        copy_nonoverlapping(
            image.code.as_ptr(),
            (ALIAS + LXE_CODE_OFFSET) as *mut u8,
            image.code.len(),
        );
        copy_nonoverlapping(
            image.data.as_ptr(),
            (ALIAS + LXE_DATA_OFFSET) as *mut u8,
            image.data.len(),
        );
        asm!("dsb sy", "ic iallu", "dsb sy", "isb");
        copy_nonoverlapping(packed.as_ptr(), git_mailbox(), packed.len());
        let status = enter_user(USER + LXE_CODE_OFFSET + image.entry, argc, USER + SCRIPT_OFFSET, flags);
        u16::try_from(status).map_err(|_| "executable fault")
    }
}

pub(crate) fn launch(image: &[u8], script: Option<(&str, &str)>, save: bool) -> Result<u8, &'static str> {
    u8::try_from(launch_status(image, script, save)?).map_err(|_| "user program fault")
}

pub(crate) fn launch_status(image: &[u8], script: Option<(&str, &str)>, save: bool) -> Result<u16, &'static str> {
    launch_segments(image, &[], script, save)
}

pub(crate) fn launch_segments(image: &[u8], data: &[u8], script: Option<(&str, &str)>, save: bool) -> Result<u16, &'static str> {
    if image.len() > SCRIPT_OFFSET || (!data.is_empty() && image.len() > BLOCK) || data.len() > UTILITY_OFFSET - BLOCK {
        return Err("user image too large");
    }
    ensure_image()?;
    unsafe {
        write_bytes(ALIAS as *mut u8, 0, USER_REGION_BYTES);
        copy_nonoverlapping(image.as_ptr(), ALIAS as *mut u8, image.len());
        copy_nonoverlapping(data.as_ptr(), (ALIAS + BLOCK) as *mut u8, data.len());
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
        let slot = bank();
        CAN_PUBLISH[slot] = core::ptr::eq(image, TCC) && !save;
        let status = enter_user(path_ptr, source_ptr, len, usize::from(save));
        CAN_PUBLISH[slot] = false;
        u16::try_from(status)
            .map_err(|_| "user program fault")
    }
}
#[unsafe(no_mangle)]
extern "C" fn report_user_fault(esr: u64, pc: u64, address: u64, sp: u64) -> u64 {
    crate::print(&alloc::format!("User fault: ESR={esr:#x} PC={pc:#x} FAR={address:#x} SP={sp:#x}\n"));
    match esr >> 26 {
        0x20 | 0x24 => 139, // instruction or data abort from EL0
        _ => 132,
    }
}

#[unsafe(no_mangle)]
extern "C" fn kernel_vector_fault() -> ! {
    crate::print("Fatal EL1 exception\n");
    crate::idle()
}

// A contiguous EL0 address range backed on demand by private, non-executable
// blocks. The normal identity mapping of the kernel heap stays privileged.
pub(crate) const ARENA_BASE: usize = 0x6800_0000;
pub(crate) const WEB_HEAP_BASE: usize = 0x7800_0000;
const PROGRAM_BASE: usize = 0xa000_0000;
pub(crate) const EXTENDED_BASE: usize = 0xc000_0000;
pub(crate) const WIDE_BASE: usize = 64 * crate::GIB;
pub(crate) const SHARED_BASE: usize = 48 * crate::GIB;
const PROGRAM_LIMIT: usize = EXTENDED_BASE - PROGRAM_BASE;
const ARENA_LIMIT: usize = 128 * 1024 * 1024;
const _: () = assert!(ARENA_BASE + ARENA_LIMIT <= ALIAS && ALIAS + USER_REGION_BYTES <= WEB_HEAP_BASE
    && WEB_HEAP_BASE + ARENA_LIMIT <= 0x8000_0000);
pub(crate) struct UserArena {
    sealed: usize,
    base: usize,
    bank: usize,
    blocks: Vec<core::ptr::NonNull<u8>>,
}
impl UserArena {
    pub fn new() -> Self {
        Self::at(ARENA_BASE)
    }
    pub fn browser_heap_limit() -> usize {
        if crate::high_heap_end().is_some() { PROGRAM_BASE - WEB_HEAP_BASE } else { ARENA_LIMIT }
    }
    pub fn for_browser_heap() -> Self {
        Self::at(WEB_HEAP_BASE)
    }
    pub fn for_program() -> Self { Self::at(PROGRAM_BASE) }
    pub fn wide() -> Self { Self::at(WIDE_BASE) }
    pub fn extended() -> Self {
        Self::at(EXTENDED_BASE)
    }
    unsafe fn entry(&self, i: usize) -> *mut u64 {
        let space = tables(self.bank);
        unsafe {
            if self.base == WEB_HEAP_BASE && WEB_HEAP_BASE + i * BLOCK >= 0x8000_0000 {
                addr_of_mut!((*space).program.0[(WEB_HEAP_BASE + i * BLOCK - 0x8000_0000) / BLOCK])
            } else if self.base == PROGRAM_BASE {
                addr_of_mut!((*space).program.0[(PROGRAM_BASE % crate::GIB) / BLOCK + i])
            } else if self.base == WIDE_BASE {
                addr_of_mut!((*space).wide[i / 512].0[i % 512])
            } else if self.base == EXTENDED_BASE {
                addr_of_mut!((*space).extended.0[i])
            } else {
                addr_of_mut!((*space).ram.0[(self.base - 0x40000000) / BLOCK + i])
            }
        }
    }
    fn at(base: usize) -> Self {
        Self {
            sealed: 0, base, bank: bank(),
            blocks: Vec::new(),
        }
    }
    pub fn set_mapped(&self, visible: bool) {
        if self.blocks.is_empty() { return; }
        for (i, block) in self.blocks.iter().enumerate() {
            unsafe { *self.entry(i) =
                if visible {block.as_ptr() as u64 | 1 | AF | SH | USER_ACCESS | PXN
                    | if i < self.sealed { READ_ONLY } else { UXN }} else {0}; }
        }
        unsafe { asm!("dsb ishst", "tlbi vmalle1", "dsb ish", "isb"); }
    }
    // Publish a loaded program's text once. The remainder stays RW and NX.
    pub fn seal_text(&mut self, bytes: usize) -> bool {
        if self.sealed != 0 || bytes == 0 || bytes % BLOCK != 0 || bytes / BLOCK > self.blocks.len() {
            return false;
        }
        self.sealed = bytes / BLOCK;
        self.set_mapped(true);
        unsafe { asm!("dsb ish", "ic iallu", "dsb ish", "isb"); }
        true
    }
    pub fn contains(&self, address:usize, bytes:usize)->bool {
        address>=self.base && address.checked_add(bytes).is_some_and(|end| end<=self.base+self.blocks.len()*BLOCK)
    }
    pub fn grow(&mut self, bytes: usize) -> Result<(), &'static str> {
        let limit = match self.base {
            WEB_HEAP_BASE if bytes > ARENA_LIMIT => Self::browser_heap_limit(),
            PROGRAM_BASE => PROGRAM_LIMIT,
            EXTENDED_BASE if bytes > ARENA_LIMIT && crate::high_heap_end().is_some() => crate::GIB,
            WIDE_BASE => 4 * crate::GIB,
            _ => ARENA_LIMIT,
        };
        if bytes > limit {
            return Err("user heap limit");
        }
        let count = bytes.div_ceil(BLOCK);
        let layout = core::alloc::Layout::from_size_align(BLOCK, BLOCK).unwrap();
        self.blocks
            .try_reserve(count.saturating_sub(self.blocks.len()))
            .map_err(|_| "out of memory")?;
        while self.blocks.len() < count {
            let block = core::ptr::NonNull::new(unsafe { alloc::alloc::alloc_zeroed(layout) })
                .ok_or("out of memory")?;
            let index = self.blocks.len();
            unsafe {
                *self.entry(index) =
                    block.as_ptr() as u64 | 1 | AF | SH | USER_ACCESS | PXN | UXN;
                asm!("dsb ishst", "tlbi vmalle1", "dsb ish", "isb");
            }
            self.blocks.push(block);
            unsafe { ARENA_BYTES[self.bank] += BLOCK; }
        }
        Ok(())
    }
}
impl Drop for UserArena {
    fn drop(&mut self) {
        if self.blocks.is_empty() { return; }
        let layout = core::alloc::Layout::from_size_align(BLOCK, BLOCK).unwrap();
        unsafe {
            for i in 0..self.blocks.len() {
                *self.entry(i) = 0;
            }
            asm!("dsb ishst", "tlbi vmalle1", "dsb ish", "isb");
            ARENA_BYTES[self.bank] -= self.blocks.len()*BLOCK;
            for block in self.blocks.drain(..) {
                alloc::alloc::dealloc(block.as_ptr(), layout);
            }
        }
    }
}

use crate::shared_memory::{LIMIT as SHARED_LIMIT, MAPS as SHARED_MAPS, PAGE, Shared};

struct SharedMapping {
    file: Shared,
    bytes: usize,
    tables: Vec<alloc::boxed::Box<Table>>,
}

impl Drop for SharedMapping {
    fn drop(&mut self) {
        self.file.borrow_mut().mappings -= 1;
    }
}

pub(crate) struct SharedMappings {
    bank: usize,
    slots: [Option<SharedMapping>; SHARED_MAPS],
}

impl SharedMappings {
    pub fn new() -> Self {
        Self { bank: bank(), slots: core::array::from_fn(|_| None) }
    }

    fn publish(&self, slot: usize, mapped: bool) {
        let Some(mapping) = &self.slots[slot] else { return; };
        let first = slot * (SHARED_LIMIT / BLOCK);
        for (i, table) in mapping.tables.iter().enumerate() {
            unsafe {
                (*tables(self.bank)).shared.0[first + i] =
                    if mapped { (&**table as *const Table as u64) | 3 } else { 0 };
            }
        }
    }

    fn flush() {
        unsafe { asm!("dsb ishst", "tlbi vmalle1", "dsb ish", "isb"); }
    }

    pub fn set_mapped(&self, mapped: bool) {
        if self.slots.iter().all(Option::is_none) { return; }
        for i in 0..self.slots.len() { self.publish(i, mapped); }
        Self::flush();
    }

    pub fn map(&mut self, file: Shared, bytes: usize, offset: usize, prot: u64) -> i64 {
        if bytes == 0 || bytes > SHARED_LIMIT || offset % PAGE != 0 || !matches!(prot, 1 | 3) {
            return -22;
        }
        let bytes = bytes.next_multiple_of(PAGE);
        let base = {
            let f = file.borrow();
            if offset.checked_add(bytes).is_none_or(|end| end > f.len.next_multiple_of(PAGE)) {
                return -22;
            }
            f.physical() + offset
        };
        let Some(slot) = self.slots.iter().position(Option::is_none) else { return -12; };
        let count = bytes.div_ceil(BLOCK);
        let mut entries = Vec::new();
        if entries.try_reserve_exact(count).is_err() { return -12; }
        for i in 0..count {
            let table = unsafe { alloc::alloc::alloc_zeroed(core::alloc::Layout::new::<Table>()).cast::<Table>() };
            if table.is_null() { return -12; }
            let mut table = unsafe { alloc::boxed::Box::from_raw(table) };
            for page in 0..(bytes - i * BLOCK).min(BLOCK) / PAGE {
                table.0[page] = (base + i * BLOCK + page * PAGE) as u64
                    | 3 | AF | SH | USER_ACCESS | PXN | UXN | if prot == 1 { READ_ONLY } else { 0 };
            }
            entries.push(table);
        }
        file.borrow_mut().mappings += 1;
        self.slots[slot] = Some(SharedMapping { file, bytes, tables: entries });
        self.publish(slot, true);
        Self::flush();
        (SHARED_BASE + slot * SHARED_LIMIT) as i64
    }

    pub fn unmap(&mut self, address: usize, bytes: usize) -> i64 {
        let Some(offset) = address.checked_sub(SHARED_BASE) else { return -22; };
        if offset % SHARED_LIMIT != 0 || bytes == 0 || bytes > SHARED_LIMIT { return -22; }
        let slot = offset / SHARED_LIMIT;
        if self.slots.get(slot).and_then(Option::as_ref)
            .is_none_or(|map| map.bytes != bytes.next_multiple_of(PAGE))
        {
            return -22;
        }
        self.publish(slot, false);
        Self::flush();
        self.slots[slot] = None;
        0
    }
}

// A fixed-address AArch64 ELF with one RX and one RW PT_LOAD. The kernel
// clears each bank before loading, including BSS and unused stack space.
pub(crate) struct ProcessImage<'a> { text:&'a [u8], data:&'a [u8] }
pub(crate) fn process_image(file:&[u8])->Option<ProcessImage<'_>>{
    split_elf(file, false)
}

// Static musl applications use the ordinary utility argument ABI and include
// a PT_TLS descriptor in addition to their RX and RW loadable segments.
pub(crate) fn application_segments(file: &[u8]) -> Option<(&[u8], &[u8])> {
    let image = split_elf(file, true)?;
    Some((image.text, image.data))
}

fn split_elf(file: &[u8], application: bool) -> Option<ProcessImage<'_>> {
    let half = |i| Some(u16::from_le_bytes(file.get(i..i+2)?.try_into().ok()?));
    let word = |i| Some(u32::from_le_bytes(file.get(i..i+4)?.try_into().ok()?));
    let wide = |i| usize::try_from(u64::from_le_bytes(file.get(i..i+8)?.try_into().ok()?)).ok();
    let count = if application { 3 } else { 2 };
    if file.get(..7)? != b"\x7fELF\x02\x01\x01" || half(16)? != 2 || half(18)? != 183
        || word(20)? != 1 || wide(24)? != USER || half(54)? != 56 || half(56)? != count
    {
        return None;
    }
    let table = wide(32)?;
    file.get(table..table.checked_add(usize::from(count) * 56)?)?;
    if application && word(table + 112)? != 7 { return None; }
    let load = |i: usize, flags, base, max| {
        if word(i)? != 1 || word(i+4)? != flags || wide(i+16)? != base { return None; }
        let (start, len, mem) = (wide(i+8)?, wide(i+32)?, wide(i+40)?);
        if len > mem || mem > max { return None; }
        file.get(start..start.checked_add(len)?)
    };
    let text = load(table, 5, USER, BLOCK)?;
    if text.len() < 4 { return None; }
    let end = if application { UTILITY_OFFSET } else { SCRIPT_OFFSET };
    Some(ProcessImage { text, data: load(table + 56, 6, USER + BLOCK, end - BLOCK)? })
}
pub(crate) fn start_process(file:&[u8],cwd:&str,flags:usize)->Result<u16,&'static str>{
    let image=process_image(file).ok_or("invalid process ELF")?;
    ensure_image()?;
    unsafe{
        write_bytes(ALIAS as *mut u8,0,USER_REGION_BYTES);
        copy_nonoverlapping(image.text.as_ptr(),ALIAS as *mut u8,image.text.len());
        copy_nonoverlapping(image.data.as_ptr(),(ALIAS+BLOCK) as *mut u8,image.data.len());
        copy_nonoverlapping(cwd.as_ptr(),git_mailbox(),cwd.len());
        asm!("dsb sy","ic iallu","dsb sy","isb");
        u16::try_from(enter_user(USER+SCRIPT_OFFSET,flags,0,0)).map_err(|_|"user program fault")
    }
}
#[cfg(feature = "retina")]
pub(crate) fn saved_mailbox(memory: &mut ImageMemory) -> &mut [u8] {
    unsafe {
        core::slice::from_raw_parts_mut(memory.0.as_ptr().add(SCRIPT_OFFSET), SCRIPT_CAPACITY)
    }
}
pub(crate) fn suspend_process() -> ImageMemory {
    replace_image(None).expect("suspending a running process")
}
pub(crate) fn resume_agent(memory: ImageMemory) -> Result<u16, &'static str> {
    resume_with_flags(memory, 65536)
}
pub(crate) fn continue_process()->Result<u16,&'static str> {
    u16::try_from(unsafe {enter_user(0,0,0,0)}).map_err(|_|"user program fault")
}
fn resume_with_flags(memory: ImageMemory, flags: usize) -> Result<u16, &'static str> {
    replace_image(Some(memory));
    u16::try_from(unsafe { enter_user(0, 0, 0, flags) }).map_err(|_| "user program fault")
}

// A small entry segment preserves the utility calling convention. A separate
// arena holds the full program, with a block-aligned boundary between RX and RW.
pub(crate) struct LargeProgram<'a> {
    pub entry: &'a [u8],
    text: &'a [u8],
    data: &'a [u8],
    writable_offset: usize,
    bytes: usize,
}
impl LargeProgram<'_> {
    pub fn load(&self, arena: &mut UserArena) -> Result<(), &'static str> {
        arena.grow(self.bytes)?;
        unsafe {
            copy_nonoverlapping(self.text.as_ptr(), PROGRAM_BASE as *mut u8, self.text.len());
            copy_nonoverlapping(self.data.as_ptr(), (PROGRAM_BASE + self.writable_offset) as *mut u8, self.data.len());
        }
        if !arena.seal_text(self.writable_offset) { return Err("cannot seal program text"); }
        Ok(())
    }
}

pub(crate) fn large_program(file: &[u8]) -> Option<LargeProgram<'_>> {
    if file.get(..7)? != b"\x7fELF\x02\x01\x01" || file.get(16..24)? != [2,0,183,0,1,0,0,0]
        || file.get(54..58)? != [56,0,4,0] { return None; }
    let wide = |s: &[u8]| usize::try_from(u64::from_le_bytes(s.try_into().ok()?)).ok();
    if wide(file.get(24..32)?)? != USER { return None; }
    let off = wide(file.get(32..40)?)?;
    let table = file.get(off..off.checked_add(4 * 56)?)?;
    if table[168..172] != 7u32.to_le_bytes() { return None; }
    let segment = |i:usize, flags:u32| {
        let p = &table[i*56..(i+1)*56];
        if p[..4] != 1u32.to_le_bytes() || p[4..8] != flags.to_le_bytes() { return None; }
        let offset = wide(&p[8..16])?;
        let size = wide(&p[32..40])?;
        let mem = wide(&p[40..48])?;
        if size > mem { return None; }
        Some((wide(&p[16..24])?, file.get(offset..offset.checked_add(size)?)?, mem))
    };
    let (start, entry, stub_size) = segment(0,5)?;
    let (rx, text, rx_size) = segment(1,5)?;
    let (rw, data, rw_size) = segment(2,6)?;
    let writable_offset = rw.checked_sub(PROGRAM_BASE)?;
    let bytes = writable_offset.checked_add(rw_size)?;
    if start != USER || entry.len() < 4 || stub_size > BLOCK || rx != PROGRAM_BASE
        || text.is_empty() || rx_size > PROGRAM_LIMIT || writable_offset != rx_size.next_multiple_of(BLOCK)
        || bytes > PROGRAM_LIMIT { return None; }
    Some(LargeProgram { entry, text, data, writable_offset, bytes })
}
