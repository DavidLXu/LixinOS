fn main() {
    if let Ok(crt) = std::env::var("LIXINOS_RUST_APP_CRT") {
        for name in ["crt0.o", "crt1.o"] {
            println!("cargo:rerun-if-changed={crt}/{name}");
        }
    }
    println!("cargo:rerun-if-changed=../rust/link.sh");
    println!("cargo:rerun-if-changed=../gcc/application.ld");
    println!("cargo:rerun-if-changed=../../Kernel/target/gcc-port/rt/liblx.a");
    let config = slint_build::CompilerConfiguration::new()
        .embed_resources(slint_build::EmbedResourcesKind::EmbedForSoftwareRenderer);
    slint_build::compile_with_config("settings.slint", config).expect("compile settings interface");
}
