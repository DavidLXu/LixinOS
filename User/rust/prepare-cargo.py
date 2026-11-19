"""Adapt Cargo's process launcher to the guest's spawn/wait ABI."""
from pathlib import Path
import sys
p = Path(sys.argv[1]) / "crates/cargo-util/src/process_builder.rs"
original = p.read_text()
s = original
# On Unix the upstream jobserver exports pipe descriptors via pre_exec.
# Cargo still keeps its local scheduler. The guest can pass only stdio.
old = """        if let Some(ref c) = self.jobserver {
            c.configure(&mut command);
        }"""
s = s.replace(old, "        // LixinOS child processes inherit only their three standard streams.")
s = s.replace("        imp::exec_replace(self)", "        self.exec()")
if s != original:
    p.write_text(s)

# Use the guest filesystem's extraction times and permissions for registry
# sources. Keep tar's path validation, exclusive opens and size checks.
root = Path(sys.argv[1])
registry = root / "src/sources/registry/mod.rs"
s = registry.read_text()
old = "        set_mask(&mut tar);"
if "tar.set_preserve_mtime(false)" not in s:
    registry.write_text(s.replace(old, old + "\n        tar.set_preserve_mtime(false);"))
tar = root.parent / "tar-0.4.46/src/entry.rs"
s = tar.read_text()
old = "            _set_perms(dst, f, mode, mask, preserve).map_err(|e| {"
new = """            let result = _set_perms(dst, f, mode, mask, preserve);
            if !preserve && result.as_ref().is_err_and(|e| e.kind() == io::ErrorKind::Unsupported) {
                return Ok(());
            }
            result.map_err(|e| {"""
if new not in s:
    if old not in s: raise ValueError("unexpected upstream tar permissions code")
    tar.write_text(s.replace(old, new))
manifest = root / "Cargo.toml"
s = manifest.read_text()
if '[patch.crates-io]' not in s:
    manifest.write_text(s + '\n[patch.crates-io]\ntar = { path = "../tar-0.4.46" }\n')
lock = root / "Cargo.lock"
s = lock.read_text()
entry = 'name = "tar"\nversion = "0.4.46"\n'
source = 'source = "registry+https://github.com/rust-lang/crates.io-index"\nchecksum = "3f6221d9a6003c78398e3b239969f352578258df48c8eb051caadae0015bc840"\n'
if entry + source in s:
    lock.write_text(s.replace(entry + source, entry))

manifest = root / "Cargo.toml"
s = manifest.read_text()
portable = 'blake3 = { version = "1.8.4", features = ["pure"] }'
if portable not in s:
    if 'blake3 = "1.8.4"' not in s: raise ValueError("unexpected BLAKE3 dependency")
    manifest.write_text(s.replace('blake3 = "1.8.4"', portable))

context = root / "src/context/mod.rs"
s = context.read_text()
old = "self.maybe_get_tool(tool_str, from_config)\n            .or_else(|| {"
new = '''self.maybe_get_tool(tool_str, from_config)
            .or_else(|| {
                if tool_str != "rustc" { return None; }
                let sibling = std::env::current_exe().ok()?.with_file_name("rustc");
                sibling.is_file().then_some(sibling)
            })
            .or_else(|| {'''
if new not in s:
    if s.count(old) != 1: raise ValueError("unexpected Cargo tool lookup")
    context.write_text(s.replace(old, new))
