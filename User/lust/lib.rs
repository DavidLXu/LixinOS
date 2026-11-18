#![no_std]
#![allow(dead_code, unused_imports)]
#[macro_use]
extern crate alloc;
mod prelude {
    pub use alloc::{borrow::ToOwned, boxed::Box, string::{String, ToString}, vec::Vec};
}
use prelude::*;
mod ast;
mod borrowck;
mod cgen;
mod check;
mod collect;
mod diag;
mod intern;
mod ir;
mod lexer;
mod lower;
mod parser;
mod types;
use core::{alloc::{GlobalAlloc, Layout}, ffi::{c_char, c_void, CStr}, panic::PanicInfo};

unsafe extern "C" {
    fn memalign(align: usize, size: usize) -> *mut u8;
    fn free(p: *mut u8);
    fn lx_error(s: *const c_char);
    fn _exit(status: i32) -> !;
    fn tcc_compile_string(state: *mut c_void, unit: *const c_char) -> i32;
}

struct Heap;
unsafe impl GlobalAlloc for Heap {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 { unsafe { memalign(l.align().max(16), l.size()) } }
    unsafe fn dealloc(&self, p: *mut u8, _: Layout) { unsafe { free(p) } }
}
#[global_allocator]
static HEAP: Heap = Heap;
#[panic_handler]
fn panic(_: &PanicInfo) -> ! {
    unsafe { lx_error(c"lust: compiler resource limit or internal error\n".as_ptr()); _exit(1) }
}

fn compile(path: &str, src: &str, state: *mut c_void) -> Result<(), String> {
    let interner = intern::Interner::default();
    let known = intern::Known::new(&interner);
    let errors = |mut ds: Vec<diag::Diag>| diag::render(&mut ds, path, src, false);
    let tokens = lexer::lex(src, 0, &interner).map_err(|d| errors(vec![d]))?;
    let ast = parser::parse(&tokens, &interner, &known).map_err(|d| errors(vec![d]))?;
    drop(tokens);
    let p = collect::collect(&ast, &interner, &known).map_err(errors)?;
    let main = p.main.ok_or("lust: missing fn main()\n")?;
    let slots: Vec<_> = (0..p.fns.len() as u32).collect();
    let lines = diag::LineIndex::new(src);
    let mut fns = Vec::new();
    for &slot in &slots {
        let id = collect::FnId(slot);
        let (checked, ds) = check::check_fn(&p, id);
        if !ds.is_empty() { return Err(errors(ds)); }
        let body = lower::lower(&p, id, checked);
        let ds = borrowck::borrowck(&p, &body);
        if !ds.is_empty() { return Err(errors(ds)); }
        fns.push(cgen::gen_fn(&p, &body, &lines, 1, &slots));
    }
    let mut glue = cgen::GlueSet::default();
    glue.add(&p, fns.iter().flat_map(|f| f.glue.iter()));
    let glue_unit = cgen::glue_unit(&p, &glue);
    let fn_unit = cgen::fn_unit(&p, &fns, 0..slots.len(), &glue, "lust_register_u0");
    // TinyCC shares this heap. Release the front-end arenas before handing
    // it large generated translation units.
    drop((fns, glue, p));
    drop((ast, interner));
    let emit = |mut unit: String| {
        unit.push('\0');
        if unsafe { tcc_compile_string(state, unit.as_ptr().cast()) } < 0 { Err(String::from("lust: TinyCC compilation failed\n")) } else { Ok(()) }
    };
    emit(format!("#define LUST_IMPL 1\n{}", include_str!("runtime.c")))?;
    emit(cgen::main_unit(main.0, path, &vec![1; slots.len()], &[]))?;
    emit(cgen::table_unit(slots.len(), &[0]))?;
    emit(glue_unit)?;
    emit(fn_unit)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lust_compile(path: *const c_char, src: *const c_char, state: *mut c_void) -> i32 {
    let path = unsafe { CStr::from_ptr(path) }.to_str();
    let src = unsafe { CStr::from_ptr(src) }.to_str();
    let result = match (path, src) {
        (Ok(path), Ok(src)) => compile(path, src, state),
        _ => Err("lust: expected UTF-8 source and filename\n".into()),
    };
    if let Err(mut msg) = result {
        msg.push('\0');
        unsafe { lx_error(msg.as_ptr().cast()) }
        return 1;
    }
    0
}
