BUILD := ../../Kernel/target/user-build
RUSTUP := $(or $(shell command -v rustup),$(HOME)/.cargo/bin/rustup)
all: $(BUILD)/sh.elf
$(BUILD)/sh.a: lib.rs main.rs execute.rs abi.rs ../foreground/keys.rs Makefile
	$(RUSTUP) run stable rustc --edition=2024 --crate-name lixinos_sh --crate-type staticlib --target aarch64-unknown-none-softfloat -C panic=abort -C opt-level=s $< -o $@
$(BUILD)/sh.elf: $(BUILD)/sh.a start.c link.ld ../console.h Makefile
	aarch64-none-elf-gcc -Os -ffunction-sections -fdata-sections -nostartfiles -Wl,--gc-sections,-T,link.ld,-s,-z,max-page-size=4096,--build-id=none start.c $< -lc -lnosys -lgcc -o $@
