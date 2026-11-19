// Run inside LixinOS. Keep rebuildable Cargo state on the build disk under /scratch.
use std::{env, fs, process::{Command, ExitCode}};

fn verify() -> Result<ExitCode, Box<dyn std::error::Error>> {
    let project = "/scratch/stage1-verify";
    let target = "/scratch/stage1-test-target";
    if std::path::Path::new(target).exists() { fs::remove_dir_all(target)?; }
    fs::create_dir_all(format!("{project}/src"))?;
    fs::write(format!("{project}/Cargo.toml"),
        "[package]\nname='stage1_verify'\nversion='0.1.0'\nedition='2024'\n")?;
    fs::write(format!("{project}/build.rs"),
        r#"fn main() { println!("cargo:rustc-env=ANSWER={}", 6 * 7); }"#)?;
    fs::write(format!("{project}/src/main.rs"),
        r#"fn main() { println!("stage1={}", env!("ANSWER")); }"#)?;
    let output = Command::new("/stage1/cargo").current_dir(project)
        .args(["run", "--offline", "--quiet"])
        .env("RUSTC", "/stage1/rustc").env("PATH", "/stage1:/bin")
        .env("CARGO_TARGET_DIR", target)
        .env("HOME", "/").env("TMPDIR", "/scratch/tmp")
        .env("COMPILER_PATH", "/lib/gcc/bin:/bin").env("CARGO_HOME", "/scratch/home")
        .env("CARGO_INCREMENTAL", "0").env("CARGO_BUILD_JOBS", "1")
        .env("CARGO_PROFILE_DEV_DEBUG", "0").output()?;
    eprint!("{}", String::from_utf8_lossy(&output.stderr));
    print!("{}", String::from_utf8_lossy(&output.stdout));
    if !output.status.success() || output.stdout != b"stage1=42\n" {
        return Err("stage1 Cargo/rustc verification failed".into());
    }
    Ok(ExitCode::SUCCESS)
}

fn run() -> Result<ExitCode, Box<dyn std::error::Error>> {
    let component = env::args().nth(1).ok_or("expected rustc, cargo, verify or stage2")?;
    if component == "verify" { return verify(); }
    let stage2 = component == "stage2";
    let tools = if stage2 { "/stage1" } else { "/bin" };
    let tool_path = if stage2 { "/stage1:/bin" } else { "/bin" };
    let (workspace, package) = match component.as_str() {
        "rustc" | "stage2" => ("rust", "rustc-main"),
        "cargo" => ("cargo", "cargo"),
        _ => return Err("expected rustc, cargo, verify or stage2".into()),
    };
    for dir in ["/scratch/home", "/scratch/tmp"] { fs::create_dir_all(dir)?; }
    let target = if stage2 {
        String::from("/scratch/stage2-target")
    } else {
        format!("/scratch/{workspace}-target")
    };
    let compiler = format!("{tools}/rustc");
    let mut cargo = Command::new(format!("{tools}/cargo"));
    cargo.current_dir(format!("/bootstrap/{workspace}"))
        .args(["build", "--release", "--offline", "--locked", "-p", package])
        .env("PATH", tool_path).env("HOME", "/").env("COMPILER_PATH", "/lib/gcc/bin:/bin")
        .env("CARGO_HOME", "/scratch/home").env("TMPDIR", "/scratch/tmp")
        .env("CARGO_TARGET_DIR", &target)
        .env("CARGO_BUILD_JOBS", "1").env("CARGO_INCREMENTAL", "0")
        .env("CARGO_PROFILE_RELEASE_OPT_LEVEL", "s").env("CARGO_PROFILE_RELEASE_DEBUG", "0")
        .env("CC", "/bin/gcc").env("AR", "/bin/ar").env("RUSTC", &compiler)
        .env("RUSTFLAGS", "--cfg rustix_use_libc -Cpanic=abort -Clink-arg=/lib/rust/rt/metadata.o -Clink-arg=-T/bootstrap/large.ld");
    if stage2 {
        cargo.arg("--verbose");
        eprintln!("building /stage2/rustc with {tools}/cargo and {compiler} in {target}");
    }
    if package == "rustc-main" {
        cargo.env("CG_CLIF_FORCE_GNU_AS", "1").env("RUSTC_BOOTSTRAP", "1").env("CFG_RELEASE", "1.100.0")
            .env("CFG_RELEASE_CHANNEL", "stable").env("CFG_VERSION", "1.100.0 (0ef1bc386 2026-11-09)")
            .env("CFG_VER_DATE", "2026-11-09").env("CFG_VER_HASH", "0ef1bc386059596dada8fb65b50425aab2ea637a")
            .env("CFG_COMPILER_HOST_TRIPLE", "aarch64-unknown-linux-musl")
            .env("RUSTC_INSTALL_BINDIR", "bin").env("CFG_DEFAULT_CODEGEN_BACKEND", "cranelift")
            .env("CFG_DEFAULT_LINKER", "/bin/gcc");
    } else {
        cargo.args(["--bin", "cargo", "--features", "vendored-libgit2,curl/static-curl"])
            .env("OPENSSL_DIR", "/bootstrap/openssl").env("OPENSSL_STATIC", "1")
            .env("CFLAGS", "-DHAVE_PIPE -DHAVE_PIPE2")
            .env("LIBSQLITE3_FLAGS", "-DSQLITE_DEFAULT_UNIX_VFS=\"unix-dotfile\"");
    }
    let result = cargo.status()?;
    if !result.success() { return Ok(ExitCode::from(result.code().unwrap_or(1) as u8)); }
    let directory = if stage2 { "/stage2" } else { "/stage1" };
    fs::create_dir_all(directory)?;
    let binary = if stage2 { "rustc" } else { &component };
    fs::rename(format!("{target}/release/{package}"), format!("{directory}/{binary}"))?;
    Ok(ExitCode::SUCCESS)
}

fn main() -> ExitCode {
    run().unwrap_or_else(|error| { eprintln!("bootstrap: {error}"); ExitCode::FAILURE })
}
