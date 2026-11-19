// Run using the installed stage0 in LixinOS. Keep rebuildable Cargo state on scratch.
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
    let component = env::args().nth(1).ok_or("expected rustc or cargo")?;
    if component == "verify" { return verify(); }
    let (workspace, package) = match component.as_str() {
        "rustc" => ("rust", "rustc-main"),
        "cargo" => ("cargo", "cargo"),
        _ => return Err("expected rustc or cargo".into()),
    };
    for dir in ["/scratch/home", "/scratch/tmp"] { fs::create_dir_all(dir)?; }
    let mut cargo = Command::new("/bin/cargo");
    cargo.current_dir(format!("/bootstrap/{workspace}"))
        .args(["build", "--release", "--offline", "--locked", "-p", package])
        .env("PATH", "/bin").env("HOME", "/").env("COMPILER_PATH", "/lib/gcc/bin:/bin")
        .env("CARGO_HOME", "/scratch/home").env("TMPDIR", "/scratch/tmp")
        .env("CARGO_TARGET_DIR", format!("/scratch/{workspace}-target"))
        .env("CARGO_BUILD_JOBS", "1").env("CARGO_INCREMENTAL", "0")
        .env("CARGO_PROFILE_RELEASE_OPT_LEVEL", "s").env("CARGO_PROFILE_RELEASE_DEBUG", "0")
        .env("CC", "/bin/gcc").env("AR", "/bin/ar").env("RUSTC", "/bin/rustc")
        .env("RUSTFLAGS", "--cfg rustix_use_libc -Cpanic=abort -Clink-arg=/lib/rust/rt/metadata.o -Clink-arg=-T/bootstrap/large.ld");
    if component == "rustc" {
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
    fs::create_dir_all("/stage1")?;
    let output = format!("/stage1/{component}");
    fs::rename(format!("/scratch/{workspace}-target/release/{package}"), output)?;
    Ok(ExitCode::SUCCESS)
}

fn main() -> ExitCode {
    run().unwrap_or_else(|error| { eprintln!("bootstrap: {error}"); ExitCode::FAILURE })
}
