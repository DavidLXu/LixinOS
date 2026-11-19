"""Adapt the compiler-side proc-macro bridge to static guest workers."""
from pathlib import Path
import shutil
import sys

root = Path(sys.argv[1])
compiler = root / "compiler"


def change(path, old, new, count=1):
    text = path.read_text()
    if new in text:
        return
    if text.count(old) != count:
        raise ValueError(f"unexpected upstream {path}: {old}")
    path.write_text(text.replace(old, new))


# Only the compiler has remote endpoints. Macro crates use the upstream
# distribution's unmodified Client and bridge ABI.
crate = compiler / "rustc_proc_macro"
bridge = crate / "src/bridge"
if not bridge.is_dir():
    shutil.copytree(root/"library/proc_macro/src", crate/"src")
change(crate/"Cargo.toml", 'path = "../../library/proc_macro/src/lib.rs"', 'path = "src/lib.rs"')
change(bridge/"mod.rs", "mod closure;", "mod closure;\n#[allow(unsafe_code)]\nmod pipe;")
pipe = bridge/"pipe.rs"
source = Path(__file__).with_name("macro-pipe.rs").read_bytes()
if not pipe.exists() or pipe.read_bytes() != source:
    pipe.write_bytes(source)
client = bridge/"client.rs"
change(client, 'pub struct Client {', """pub struct Client {
    pub(super) remote: Option<(&'static str, usize)>,""")
change(client, '            run: super::selfless_reify', '            remote: None,\n            run: super::selfless_reify', 2)
change(client, 'impl Client {', '''impl Client {
    #[doc(hidden)]
    pub fn remote(path: &'static str, index: usize) -> Self {
        let mut client = Self::expand1(|stream| stream);
        client.remote = Some((path, index));
        client
    }
''')
server = bridge/"server.rs"
change(server, 'use std::cell::Cell;', 'include!("remote.rs");\nuse std::cell::Cell;')
dest = bridge/"remote.rs"
src = Path(__file__).with_name("macro-server.rs").read_bytes()
if not dest.exists() or dest.read_bytes() != src:
    dest.write_bytes(src)
change(server, '    input: I,\n    run_client:', "    input: I,\n    remote: Option<(&'static str, usize)>,\n    run_client:")
change(server, 'buf = strategy.run_bridge_and_client(&mut dispatcher, buf, run_client, force_show_panics);',
    '''buf = if let Some((path, index)) = remote {
        remote_expand(&mut dispatcher, buf, path, index, force_show_panics)
            .map_err(|e| PanicMessage::String(e.to_string()))?
    } else {
        strategy.run_bridge_and_client(&mut dispatcher, buf, run_client, force_show_panics)
    };''')
change(server, 'let client::Client { run } = *self;', 'let client::Client { remote, run } = *self;', 2)
change(server, '::mark(input), run, force_show_panics)', '::mark(input), remote, run, force_show_panics)')
change(server, '            run,\n            force_show_panics,', '            remote,\n            run,\n            force_show_panics,')
loader = compiler/"rustc_metadata/src/host_dylib.rs"
text = loader.read_text()
idx = text.index('    let sym_name', text.index('pub(crate) fn dlsym_proc_macros(')) if '    let sym_name' in text[text.index('pub(crate) fn dlsym_proc_macros('):] else None
if idx is not None:
    end = text.index('\n}', idx)
    loader.write_text(text[:idx] + '''    let _ = stable_crate_id;
    let error = |e: String| DylibError::DlOpen(path.display().to_string(), e);
    let output = std::process::Command::new(path).arg("--macro-count").output()
        .map_err(|e| error(e.to_string()))?;
    if !output.status.success() { return Err(error(format!("macro worker: {}", output.status))); }
    let count = std::str::from_utf8(&output.stdout).ok().and_then(|s| s.trim().parse::<usize>().ok())
        .filter(|n| *n <= 65536).ok_or_else(|| error("invalid macro table".into()))?;
    let path: &'static str = path.to_str().ok_or_else(|| error("invalid macro path".into()))?.to_owned().leak();
    Ok((0..count).map(|index| ProcMacroClient::remote(path, index)).collect::<Vec<_>>().leak())
''' + text[end:])
output = compiler/"rustc_session/src/output.rs"
change(output, '    if let CrateType::Cdylib | CrateType::Dylib | CrateType::ProcMacro = crate_type {',
       '    if crate_type == CrateType::ProcMacro { return false; }\n    if let CrateType::Cdylib | CrateType::Dylib = crate_type {')
linker = compiler/"rustc_codegen_ssa/src/back/link.rs"
change(linker, '    let kind = match (crate_type, sess.crt_static', '    if crate_type == CrateType::ProcMacro { return LinkOutputKind::StaticNoPicExe; }\n    let kind = match (crate_type, sess.crt_static')
change(linker, '    cmd.export_symbols(tmpdir, crate_type, &export_symbols);', '''    if crate_type == CrateType::ProcMacro {
        let table = export_symbols.iter().find(|sym| sym.name.contains("__rustc_proc_macro_decls_")).expect("macro declaration table");
        cmd.link_arg(format!("--defsym=lx_macro_table={}", table.name));
        cmd.link_or_cc_arg("/lib/rust/rt/macro-client.o");
    } else {
        cmd.export_symbols(tmpdir, crate_type, &export_symbols);
    }''')
