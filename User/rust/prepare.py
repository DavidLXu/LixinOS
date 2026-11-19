"""Configure the pinned upstream compiler with a built-in AArch64 Cranelift backend."""
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
compiler = root / "compiler"

def replace(path, old, new):
    data = path.read_text()
    if new in data:
        return
    if data.count(old) != 1:
        raise ValueError(f"unexpected upstream source: {path}")
    path.write_text(data.replace(old, new))

# The release archive omits unrelated tool submodules. Build just rustc and
# its path dependencies, leaving Cargo's source and workspace independent.
(root / "Cargo.toml").write_text('[workspace]\nresolver = "2"\nmembers = ["compiler/rustc"]\n')
backend = compiler / "rustc_codegen_cranelift"
replace(backend / "src/lib.rs", "#[allow(unused_extern_crates)]\nextern crate rustc_driver;", "// The backend is linked into this driver statically.")
for crate in [backend, compiler / "rustc_driver"]:
    replace(crate / "Cargo.toml", 'crate-type = ["dylib"]', 'crate-type = ["rlib"]')
replace(backend / "Cargo.toml", '"all-native-arch"', '"arm64"')
private = re.findall(r'^extern crate (rustc_\w+);', (backend / "src/lib.rs").read_text(), re.M)
deps = ''.join(f'{name} = {{ path = "../{name}" }}\n' for name in private)
replace(backend / "Cargo.toml", "[dependencies]\n", "[dependencies]\n" + deps)
interface = compiler / "rustc_interface"
replace(interface / "Cargo.toml", "[dependencies]\n", '[dependencies]\nrustc_codegen_cranelift = { path = "../rustc_codegen_cranelift" }\n')
replace(interface / "src/util.rs", '"dummy" => ||', '"cranelift" => rustc_codegen_cranelift::__rustc_codegen_backend,\n            "dummy" => ||')
filesearch = compiler / "rustc_session/src/filesearch.rs"
replace(filesearch, 'default_from_rustc_driver_dll().expect("Failed finding sysroot")',
        'default_from_rustc_driver_dll().unwrap_or_else(|_| PathBuf::from("/lib/rust"))')
spec = compiler / "rustc_target/src/spec/targets/aarch64_unknown_linux_musl.rs"
replace(spec, 'base.crt_static_default = true;', '''base.crt_static_default = true;
    base.linker = Some("/bin/gcc".into());
    base.link_self_contained = crate::spec::LinkSelfContainedDefault::False;
    base.position_independent_executables = false;
    base.static_position_independent_executables = false;
    base.dynamic_linking = false;
    base.no_default_libraries = false;
    base.relro_level = crate::spec::RelroLevel::None;
    base.relocation_model = crate::spec::RelocModel::Static;
    base.panic_strategy = crate::spec::PanicStrategy::Abort;''')
mapped = compiler / "rustc_data_structures/src/memmap.rs"
adapted = Path(__file__).with_name("memmap.rs").read_bytes()
if mapped.read_bytes() != adapted:
    mapped.write_bytes(adapted)
replace(compiler / "rustc_data_structures/Cargo.toml", "[dependencies]\n", '[dependencies]\nflate2 = "1.1"\n')
# The static driver uses panic=abort. Emit the same status as the upstream
# FatalErrorMarker catcher for source errors on this non-unwinding host.
replace(compiler / "rustc_span/src/fatal_error.rs",
        "std::panic::resume_unwind(Box::new(FatalErrorMarker))", "std::process::exit(1)")

# GCC 4.8 is fixed-address by default and predates the -no-pie option.
linker = compiler / "rustc_codegen_ssa/src/back/linker.rs"
replace(linker, """                self.link_or_cc_arg("-static");
                // noop on windows w/ gcc, warning w/ clang
                if !self.is_ld && self.is_gnu && !self.sess.target.is_like_windows {""",
    """                self.link_or_cc_arg("-static");
                if self.sess.target.position_independent_executables && !self.is_ld && self.is_gnu && !self.sess.target.is_like_windows {""")

replace(spec, '&["-Wl,--fix-cortex-a53-843419"]',
        '&["-nostartfiles", "/lib/rust/rt/crt0.o", "/lib/gcc/crt1.o", "/lib/rust/rt/posix.o", "/lib/rust/rt/spawn.o", "-Wl,--fix-cortex-a53-843419"]')

replace(compiler / "rustc_metadata/src/locator.rs", "let mmap = unsafe { Mmap::map(file) };",
        "let mmap = Mmap::map_metadata(filename, file);")

# Use BLAKE3's upstream portable implementation with the guest C SDK.
replace(compiler / "rustc_span/Cargo.toml", 'blake3 = "1.5.2"',
        'blake3 = { version = "1.5.2", features = ["pure"] }')

# Rust #[target_feature] functions can contain optional instruction encodings.
# GNU as accepts them here. Cranelift still selects baseline code by default.
globasm = backend / "src/global_asm.rs"
replace(globasm, 'Command::new(&config.assembler)\n            .arg("-o")',
        'Command::new(&config.assembler)\n            .arg("-march=all")\n            .arg("-o")')
asm = backend / "src/inline_asm.rs"
old = "if self.arch == InlineAsmArch::AArch64 {\n            for feature"
new = 'if self.arch == InlineAsmArch::AArch64 && option_env!("CG_CLIF_FORCE_GNU_AS").is_none() {\n            for feature'
s = asm.read_text()
if new not in s:
    if s.count(old) != 2: raise ValueError("unexpected AArch64 inline asm feature handling")
    asm.write_text(s.replace(old, new))
