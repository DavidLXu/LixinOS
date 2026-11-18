BUILD := ../../Kernel/target/user-build
RUSTUP := $(or $(shell command -v rustup),$(HOME)/.cargo/bin/rustup)
TARGET := aarch64-unknown-none-softfloat
all: $(BUILD)/foreground.bin
$(BUILD)/foreground.a: lib.rs keys.rs nano.rs Makefile
	$(RUSTUP) run stable rustc --edition=2024 --crate-name foreground --crate-type staticlib --target $(TARGET) -C panic=abort -C opt-level=s $< -o $@
$(BUILD)/foreground.bin: $(BUILD)/foreground.a runtime.c runtime.h htop.c $(wildcard ../kilo/*.c ../paint/*.c ../paint/*.h) ../user.ld ../console.h Makefile
	aarch64-none-elf-gcc -Os -ffunction-sections -fdata-sections -nostartfiles -Wl,--gc-sections,-T,../user.ld runtime.c htop.c ../kilo/kilo.c ../paint/*.c $< -lm -lc -lnosys -lgcc -o $(BUILD)/foreground.elf
	aarch64-none-elf-objcopy -O binary $(BUILD)/foreground.elf $@
