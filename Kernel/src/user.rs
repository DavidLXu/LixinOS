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
const ALIAS: usize = 0x70000000;
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
    interrupt: fn() -> Option<&'static str>,
}

// The foreground EL0 slot is single-core and uses synchronous syscalls.
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
    let stopped = if operation == 3 || operation == 0 { (io.interrupt)() } else { None };
    let stopped = if stopped.is_some() {
        stopped
    } else if test_clock() >= io.deadline {
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

fn setup_bank(bank: usize) {
    unsafe {
        // MAIR slot 0 = Normal non-cacheable, slot 1 = Device-nGnRnE.
        ROOT[bank].0[0] = 1 | AF | 4 | PXN | UXN;
        ROOT[bank].0[1] = addr_of_mut!(RAM[bank]) as u64 | 3;
        for i in 0..crate::MAPPED_RAM / BLOCK {
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
pub(crate) fn copy_user_file(descriptor: u64) -> Option<Vec<u8>> {
    let size = descriptor as u32 as usize;
    if size > crate::disk::FILE_LIMIT { return None; }
    // Only the active bank's writable memory below its mailboxes is readable.
    let bank = unsafe { core::slice::from_raw_parts((ALIAS + BLOCK) as *const u8, UTILITY_OFFSET - BLOCK) };
    let buffer = |address: usize, length: usize| {
        let start = address.checked_sub(USER + BLOCK)?;
        bank.get(start..start.checked_add(length)?)
    };
    let word = |bytes: &[u8]| u32::from_le_bytes(bytes.try_into().unwrap()) as usize;
    let address = (descriptor >> 32) as usize;
    let count = word(buffer(address, 4)?);
    // Nonempty parts contribute at least one byte apiece.
    if count > size { return None; }
    let table = buffer(address.checked_add(4)?, count.checked_mul(12)?)?;
    let mut file = alloc::vec![0; size];
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
    if bytes.len() < 16 || &bytes[..4] != b"LXE1" {
        return Err("invalid LixinOS executable");
    }
    let field = |i: usize| u32::from_le_bytes(bytes[i..i + 4].try_into().unwrap()) as usize;
    let (entry, code, data) = (field(4), field(8), field(12));
    let end = 16 + code + data;
    if end > bytes.len() || end > crate::disk::FILE_LIMIT
        || code > BLOCK - LXE_CODE_OFFSET || data > UTILITY_OFFSET - LXE_DATA_OFFSET
        || entry % 4 != 0 || entry + 4 > code {
        return Err("invalid executable layout");
    }
    Ok(Executable {
        entry,
        code: &bytes[16..16 + code],
        data: &bytes[16 + code..end],
    })
}

pub fn is_executable(bytes: &[u8]) -> bool {
    executable(bytes).is_ok_and(|image| 16 + image.code.len() + image.data.len() == bytes.len())
}

pub fn compile(path: &str, source: &str) -> Result<Vec<u8>, &'static str> {
    if launch(TCC, Some((path, source)), true)? != 0 { return Err("compilation failed"); }
    // The EL0 call has returned and the foreground slot is exclusively ours.
    let descriptor = unsafe { git_mailbox().cast::<u64>().read() };
    let bytes = copy_user_file(descriptor).ok_or("invalid compiler result buffer")?;
    if !is_executable(&bytes) { return Err("invalid compiler output"); }
    Ok(bytes)
}

pub(crate) fn start_saved(bytes: &[u8], argc: usize, packed: &str, flags: usize) -> Result<u16, &'static str> {
    let image = executable(bytes)?;
    if 16 + image.code.len() + image.data.len() != bytes.len() {
        return Err("unexpected executable length");
    }
    if packed.len() > SCRIPT_CAPACITY || argc > 512 { return Err("arguments too large"); }
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
        let slot = bank();
        CAN_PUBLISH[slot] = core::ptr::eq(image, TCC) && !save;
        let status = enter_user(path_ptr, source_ptr, len, usize::from(save));
        CAN_PUBLISH[slot] = false;
        u16::try_from(status)
            .map_err(|_| "user program fault")
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
pub(crate) struct UserArena {
    bank: usize,
    blocks: Vec<core::ptr::NonNull<u8>>,
}
impl UserArena {
    pub fn new() -> Self {
        Self {
            bank: bank(),
            blocks: Vec::new(),
        }
    }
    #[cfg(feature="retina")]
    pub fn map_canvas(&self, visible: bool) {
        for (i, block) in self.blocks.iter().enumerate() {
            unsafe { RAM[self.bank].0[(ARENA_BASE-0x40000000)/BLOCK+i] =
                if visible {block.as_ptr() as u64 | 1 | AF | SH | USER_ACCESS | PXN | UXN} else {0}; }
        }
        unsafe { asm!("dsb ishst", "tlbi vmalle1", "dsb ish", "isb"); }
    }
    #[cfg(feature="retina")]
    pub fn contains(&self, address:usize, bytes:usize)->bool {
        address>=ARENA_BASE && address.checked_add(bytes).is_some_and(|end| end<=ARENA_BASE+self.blocks.len()*BLOCK)
    }
    pub fn grow(&mut self, bytes: usize) -> Result<(), &'static str> {
        if bytes > 128 * 1024 * 1024 {
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
            let index = (ARENA_BASE - 0x40000000) / BLOCK + self.blocks.len();
            unsafe {
                RAM[self.bank].0[index] =
                    block.as_ptr() as u64 | 1 | AF | SH | USER_ACCESS | PXN | UXN;
                asm!("dsb ishst", "tlbi vmalle1", "dsb ish", "isb");
            }
            self.blocks.push(block);
        }
        Ok(())
    }
}
impl Drop for UserArena {
    fn drop(&mut self) {
        let layout = core::alloc::Layout::from_size_align(BLOCK, BLOCK).unwrap();
        unsafe {
            for i in 0..self.blocks.len() {
                RAM[self.bank].0[(ARENA_BASE - 0x40000000) / BLOCK + i] = 0;
            }
            asm!("dsb ishst", "tlbi vmalle1", "dsb ish", "isb");
            for block in self.blocks.drain(..) {
                alloc::alloc::dealloc(block.as_ptr(), layout);
            }
        }
    }
}

// A fixed-address AArch64 ELF with one RX and one RW PT_LOAD. The kernel
// clears each bank before loading, including BSS and unused stack space.
pub(crate) struct ProcessImage<'a> { text:&'a [u8], data:&'a [u8] }
pub(crate) fn process_image(file:&[u8])->Option<ProcessImage<'_>>{
    let half=|i|Some(u16::from_le_bytes(file.get(i..i+2)?.try_into().ok()?));
    let word=|i|Some(u32::from_le_bytes(file.get(i..i+4)?.try_into().ok()?));
    let wide=|i|usize::try_from(u64::from_le_bytes(file.get(i..i+8)?.try_into().ok()?)).ok();
    if file.get(..7)?!=b"\x7fELF\x02\x01\x01" || half(16)?!=2 || half(18)?!=183 || word(20)?!=1 || wide(24)?!=USER || half(54)?!=56 || half(56)?!=2 {return None;}
    let table=wide(32)?;
    file.get(table..table.checked_add(112)?)?;
    let load=|i:usize,flags,base,max|{
        if word(i)?!=1 || word(i+4)?!=flags || wide(i+16)?!=base{return None;}
        let (start,len,mem)=(wide(i+8)?,wide(i+32)?,wide(i+40)?);
        if len>mem||mem>max{return None;}
        file.get(start..start.checked_add(len)?)
    };
    let text=load(table,5,USER,BLOCK)?;
    if text.len()<4{return None;}
    Some(ProcessImage{text,data:load(table+56,6,USER+BLOCK,SCRIPT_OFFSET-BLOCK)?})
}
pub(crate) fn start_process(file:&[u8],cwd:&str,flags:usize)->Result<u16,&'static str>{
    let image=process_image(file).ok_or("invalid process ELF")?;
    unsafe{
        write_bytes(ALIAS as *mut u8,0,USER_REGION_BYTES);
        copy_nonoverlapping(image.text.as_ptr(),ALIAS as *mut u8,image.text.len());
        copy_nonoverlapping(image.data.as_ptr(),(ALIAS+BLOCK) as *mut u8,image.data.len());
        copy_nonoverlapping(cwd.as_ptr(),git_mailbox(),cwd.len());
        asm!("dsb sy","ic iallu","dsb sy","isb");
        u16::try_from(enter_user(USER+SCRIPT_OFFSET,flags,0,0)).map_err(|_|"user program fault")
    }
}
#[cfg(feature="retina")]
pub(crate) fn saved_mailbox(memory: &mut [u8]) -> &mut [u8] { &mut memory[SCRIPT_OFFSET-BLOCK..] }
pub(crate) fn suspend_process()->Vec<u8>{
    unsafe{core::slice::from_raw_parts((ALIAS+BLOCK) as *const u8,BLOCK)}.to_vec()
}
pub(crate) fn resume_process(file:&[u8],memory:&[u8])->Result<u16,&'static str>{
    resume_with_flags(file, memory, 0)
}

pub(crate) fn resume_agent(file: &[u8], memory: &[u8]) -> Result<u16, &'static str> {
    resume_with_flags(file, memory, 65536)
}

fn resume_with_flags(file: &[u8], memory: &[u8], flags: usize) -> Result<u16, &'static str> {

    unsafe{
        write_bytes(ALIAS as *mut u8,0,BLOCK);
        if let Some(image) = process_image(file) {
            copy_nonoverlapping(image.text.as_ptr(), ALIAS as *mut u8, image.text.len());
        } else if let Some(code) = crate::utility::segment(file) {
            copy_nonoverlapping(code.as_ptr(), ALIAS as *mut u8, code.len());
        } else {
            let image = executable(file)?;
            // Initialized data in RUNNER is already in the suspended RW bank.
            copy_nonoverlapping(RUNNER.as_ptr(), ALIAS as *mut u8, RUNNER.len().min(BLOCK));
            copy_nonoverlapping(image.code.as_ptr(), (ALIAS + LXE_CODE_OFFSET) as *mut u8, image.code.len());
        }
        copy_nonoverlapping(memory.as_ptr(),(ALIAS+BLOCK) as *mut u8,BLOCK);
        asm!("dsb sy","ic iallu","dsb sy","isb");
        u16::try_from(enter_user(0,0,0,flags)).map_err(|_|"user program fault")
    }
}
