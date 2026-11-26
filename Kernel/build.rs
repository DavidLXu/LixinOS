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
        } else if matches!(guest.as_str(), "/src/tcc/embed-gui.py" | "/src/agent/chat.md" | "/src/agent/native.py" | "/src/agent/desktop.py" | "/src/agent/desktop.md") || guest == "/src/games/snake.py" || guest == "/src/links/COPYING" || guest == "/src/ssh/prepare-sunset.py" || guest == "/src/links/prepare.py" || guest == "/src/links/webdeps.py" || guest == "/src/links/events.js" || matches!(
            path.extension().and_then(|s| s.to_str()),
            Some("c" | "h" | "ld" | "def" | "rs" | "swift")
        ) || matches!(
            path.file_name().and_then(|s| s.to_str()),
            Some("Makefile" | "README.md" | "Cargo.toml" | "Cargo.lock")
        ) {
            assert!(
                fs::metadata(&path).unwrap().len() <= 100 * 1024 * 1024,
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

fn kernel_sources(dir: &Path, guest: &str, entries: &mut String) {
    writeln!(entries, "({guest:?}, None),").unwrap();
    let mut files = fs::read_dir(dir).unwrap().map(|f| f.unwrap().path()).collect::<Vec<_>>();
    files.sort();
    for path in files {
        let name = path.file_name().unwrap().to_str().unwrap();
        let target = format!("{guest}/{name}");
        if path.is_dir() {
            kernel_sources(&path, &target, entries);
        } else if matches!(path.extension().and_then(|x| x.to_str()), Some("h" | "c" | "S"))
            || matches!(name, "sources" | "loader-sources" | "COPYING") {
            writeln!(entries, "({target:?}, Some(include_bytes!({:?}))),", path.canonicalize().unwrap()).unwrap();
        }
    }
}

fn main() {
    // The C kernel and Rust kernel share the revision engraved in a release.
    let version = include_str!("native/version.h").lines()
        .find_map(|line| line.strip_prefix("#define KERNEL_TAG \"")?.strip_suffix('"'))
        .expect("missing KERNEL_TAG in native/version.h");
    println!("cargo:rustc-env=LIXINOS_KERNEL_TAG={version}");
    let root = Path::new("../User");
    let mut entries = String::from("&[(\"/src\", None),\n");
    source_manifest(root, root, &mut entries);
    let runtime = std::env::current_dir().unwrap().join("target/user-build/lxe-runtime.elf");
    entries.push_str("(\"/boot\", None),\n(\"/lib\", None),\n");
    writeln!(entries, "({:?}, Some(include_bytes!({:?}))),", "/lib/lxe-runtime.elf", runtime).unwrap();
    let git = std::env::current_dir().unwrap().join("target/user-build/git-user.elf");
    writeln!(entries, "({:?}, Some(include_bytes!({:?}))),", "/lib/git.elf", git).unwrap();
    let foreground = std::env::current_dir().unwrap().join("target/user-build/foreground-user.elf");
    writeln!(entries, "({:?}, Some(include_bytes!({:?}))),", "/lib/foreground.elf", foreground).unwrap();
    entries.push_str("(\"/lib/agent-test.bin\", Some(crate::user::PYTHON)),\n");
    entries.push_str("(\"/lib/agent-io\", Some(b\"LXcmd1\\nagent-io\")),\n");
    entries.push_str("(\"/lib/agent-command\", Some(b\"LXcmd1\\nagent-command\")),\n");
    let atlas = Path::new("target/user-build/desktop.gray");
    writeln!(entries, "({:?}, Some(include_bytes!({:?}))),",
        "/src/gui/menlo.gray", std::env::current_dir().unwrap().join(atlas)).unwrap();
    let han = std::env::current_dir().unwrap().join("target/user-build/links-web/han.font");
    writeln!(entries, "({:?}, Some(include_bytes!({:?}))),", "/src/links/han.font", han).unwrap();
    let dom = std::env::current_dir().unwrap().join("target/user-build/links-web/dom.js");
    writeln!(entries, "({:?}, Some(include_bytes!({:?}))),", "/src/links/dom.js", dom).unwrap();
    // Both graphical kernels consume the same physical-pixel console atlas.
    println!("cargo:rerun-if-changed=../scripts/console-font.swift");
    let atlas = std::path::PathBuf::from(std::env::var_os("OUT_DIR").unwrap()).join("font.gray");
    assert!(Command::new("swift").arg("../scripts/console-font.swift").arg(&atlas)
        .status().expect("generate Menlo glyphs").success());
    writeln!(entries, "({:?}, Some(include_bytes!({:?}))),",
        "/src/gui/console-menlo.gray", atlas).unwrap();
    for path in [
        "../User",
        "../scripts/build-user.sh",
        "../scripts/tcc-sources.py",
        "../scripts/python-sources.py",
        "linker.ld",
        "native",
        "target/user-build/git.bin",
        "target/user-build/foreground.bin",
        "target/user-build/sh.elf",
        "target/user-build/ssh.elf",
        "target/user-build/sftp.elf",
        "target/user-build/lust.elf",
        "target/user-build/agent-run.elf",
        "target/user-build/ls.elf",
        "target/user-build/links.elf",
        "target/user-build/desktop.elf",
        "target/user-build/nxeyes.elf",
        "target/user-build/stopwatch.elf",
        "target/user-build/echo.bin",
        "target/user-build/python.bin",
        "target/user-build/tcc.bin",
        "target/user-build/runner.bin",
    ] {
        println!("cargo:rerun-if-changed={path}");
    }
    for name in ["cat","echo","grep","wc","clear","pwd","netinfo","cpuinfo","sysinfo","uname","head","tail","tee","true","false","sleep","date","sort","uniq","find","ps","df","stat","tree","du","cmp","diff","nl","cut","tr","sed","seq","basename","dirname","test","printf"] {
        println!("cargo:rerun-if-changed=target/user-build/core-{name}.elf");
    }
    let status = Command::new("bash")
        .arg("../scripts/build-user.sh")
        .status()
        .expect("start ARM64 user program build");
    assert!(status.success(), "failed to build ARM64 user programs");
    // Build the same CPU rendezvous and exception-frame ABI as the guest TCC kernel.
    let cc = fs::read_dir("target/user-build").unwrap().filter_map(Result::ok)
        .map(|entry| entry.path().join("bin/aarch64-none-elf-gcc"))
        .find(|cc| cc.is_file()).unwrap_or_else(|| "aarch64-none-elf-gcc".into());
    for source in ["smp.c", "vm.c", "swap.c", "identity.c", "record-lock.c", "interp.c", "audio.c", "kthread.c", "smp-arch.S", "gpu.c", "boot-splash.c"] {
        if matches!(source, "gpu.c" | "boot-splash.c") && std::env::var_os("CARGO_FEATURE_RETINA").is_none() { continue; }
        let object = Path::new(&std::env::var_os("OUT_DIR").unwrap()).join(format!("{source}.o"));
        assert!(Command::new(&cc).args(["-c", "-O2", "-mgeneral-regs-only", "-ffreestanding",
            "-fno-stack-protector", "-fno-builtin", "-o"]).arg(&object)
            .arg(Path::new("native").join(source)).status().unwrap().success());
        println!("cargo:rustc-link-arg={}", object.display());
    }
    kernel_sources(Path::new("native"), "/src/kernel", &mut entries);
    // A release imported from a guest carries its exact lwIP sources in native/lwip.
    if !Path::new("native/lwip").is_dir() {
        let lwip = Path::new("target/user-build/micropython/lib/lwip");
        entries.push_str("(\"/src/kernel/lwip\", None),\n");
        writeln!(entries, "({:?}, Some(include_bytes!({:?}))),", "/src/kernel/lwip/COPYING",
            lwip.join("COPYING").canonicalize().unwrap()).unwrap();
        kernel_sources(&lwip.join("src/include"), "/src/kernel/lwip/include", &mut entries);
        let mut dirs = std::collections::BTreeSet::new();
        for line in include_str!("native/sources").lines() {
            if let Some(source) = line.strip_prefix("/src/kernel/lwip/") {
                let path = Path::new(line);
                for parent in path.ancestors().skip(1).collect::<Vec<_>>().into_iter().rev() {
                    if parent.starts_with("/src/kernel/lwip") && parent != Path::new("/src/kernel/lwip")
                        && dirs.insert(parent.to_owned()) {
                        writeln!(entries, "({:?}, None),", parent).unwrap();
                    }
                }
                writeln!(entries, "({:?}, Some(include_bytes!({:?}))),", line,
                    lwip.join("src").join(source).canonicalize().unwrap()).unwrap();
            }
        }
    }
    entries.push_str(&fs::read_to_string("target/user-build/micropython-sources.rs").unwrap());
    entries.push_str(&fs::read_to_string("target/user-build/tcc-sources.rs").unwrap());
    entries.push(']');
    let out_dir = std::env::var_os("OUT_DIR").unwrap();
    fs::write(Path::new(&out_dir).join("user-sources.rs"), entries).unwrap();
}
