BUILD := ../../Kernel/target/user-build
OUT := $(BUILD)/ssh-build
RUSTUP := $(or $(shell command -v rustup),$(HOME)/.cargo/bin/rustup)
CC := aarch64-none-elf-gcc
TARGET := aarch64-unknown-none-softfloat
LIB := $(OUT)/$(TARGET)/release/liblixin_ssh.a
all: $(BUILD)/ssh.elf
$(LIB): Cargo.toml Cargo.lock src/lib.rs Makefile
	env -u CARGO_ENCODED_RUSTFLAGS RUSTFLAGS='--cfg getrandom_backend="custom" --cfg sha2_backend="soft" --cfg aes_backend="soft"' $(RUSTUP) run stable cargo build --release --locked --target $(TARGET) --target-dir $(OUT)
$(BUILD)/ssh.elf: $(LIB) port.c ../tcc/api.c ../tcc/lixin.h ../console.h ../lust/link.ld ../ls/link.ld Makefile
	$(CC) -Os -ffunction-sections -fdata-sections -nostartfiles -Wl,--gc-sections,-T,../lust/link.ld port.c ../tcc/api.c $(LIB) -lc -lnosys -lgcc -o $(OUT)/linked.elf
	aarch64-none-elf-objcopy -O binary $(OUT)/linked.elf $(OUT)/image.bin
	echo '.section .text.start,"ax"; .global _start; _start:; .incbin "$(OUT)/image.bin"' | $(CC) -x assembler -nostdlib -Wl,-T,../ls/link.ld,-s,-z,max-page-size=4096,--build-id=none -o $@.tmp -
	test $$(wc -c < $@.tmp) -le 1048576
	mv $@.tmp $@
