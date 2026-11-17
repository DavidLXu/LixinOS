use std::process::Command;

fn main() {
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
        "target/user-build/echo.bin",
        "target/user-build/python.bin",
        "target/user-build/tcc.bin",
        "target/user-build/runner.bin",
    ] {
        println!("cargo:rerun-if-changed={path}");
    }
    let status = Command::new("bash")
        .arg("../scripts/build-user.sh")
        .status()
        .expect("start ARM64 user program build");
    assert!(status.success(), "failed to build ARM64 user programs");
}
