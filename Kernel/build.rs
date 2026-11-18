use std::{fmt::Write, fs, path::Path, process::Command};

// Mirror the project-owned freestanding sources and build inputs, in parent-first order.
fn source_manifest(dir: &Path, root: &Path, entries: &mut String) {
    let mut paths: Vec<_> = fs::read_dir(dir)
        .unwrap()
        .map(|entry| entry.unwrap().path())
        .collect();
    paths.sort();
    for path in paths {
        let guest = format!("/src/{}", path.strip_prefix(root).unwrap().display());
        if path.is_dir() {
            writeln!(entries, "({guest:?}, None),").unwrap();
            source_manifest(&path, root, entries);
        } else if matches!(
            path.extension().and_then(|s| s.to_str()),
            Some("c" | "h" | "ld" | "def" | "rs" | "swift")
        ) || matches!(
            path.file_name().and_then(|s| s.to_str()),
            Some("Makefile" | "README.md" | "Cargo.toml" | "Cargo.lock")
        ) {
            assert!(
                fs::metadata(&path).unwrap().len() <= 1024 * 1024,
                "{guest} exceeds guest file limit"
            );
            writeln!(
                entries,
                "({guest:?}, Some(include_bytes!({:?}))),",
                path.canonicalize().unwrap()
            )
            .unwrap();
        }
    }
}

fn main() {
    let root = Path::new("../User");
    let mut entries = String::from("&[(\"/src\", None),\n");
    source_manifest(root, root, &mut entries);
    entries.push(']');
    let out_dir = std::env::var_os("OUT_DIR").unwrap();
    fs::write(Path::new(&out_dir).join("user-sources.rs"), entries).unwrap();

    if std::env::var_os("CARGO_FEATURE_RETINA").is_some() {
        println!("cargo:rerun-if-changed=../scripts/console-font.swift");
        let atlas =
            std::path::PathBuf::from(std::env::var_os("OUT_DIR").unwrap()).join("font.gray");
        assert!(
            Command::new("swift")
                .arg("../scripts/console-font.swift")
                .arg(atlas)
                .status()
                .expect("generate Menlo glyphs")
                .success()
        );
    }
    for path in [
        "../User",
        "../scripts/build-user.sh",
        "linker.ld",
        "target/user-build/git.bin",
        "target/user-build/foreground.bin",
        "target/user-build/sh.elf",
        "target/user-build/ssh.elf",
        "target/user-build/lust.elf",
        "target/user-build/agent-run.elf",
        "target/user-build/ls.elf",
        "target/user-build/desktop.elf",
        "target/user-build/echo.bin",
        "target/user-build/python.bin",
        "target/user-build/tcc.bin",
        "target/user-build/runner.bin",
    ] {
        println!("cargo:rerun-if-changed={path}");
    }
    for name in ["cat","echo","grep","wc","clear","pwd","netinfo"] {
        println!("cargo:rerun-if-changed=target/user-build/core-{name}.elf");
    }
    let status = Command::new("bash")
        .arg("../scripts/build-user.sh")
        .status()
        .expect("start ARM64 user program build");
    assert!(status.success(), "failed to build ARM64 user programs");
}
