BUILD := ../../Kernel/target/user-build
OUT := $(BUILD)/ssh-build
RUSTUP := $(or $(shell command -v rustup),$(HOME)/.cargo/bin/rustup)
CC := aarch64-none-elf-gcc
TARGET := aarch64-unknown-none-softfloat
LIB := $(OUT)/$(TARGET)/release/liblixin_ssh.a
SUNSET := $(OUT)/sunset-bulk
all: $(BUILD)/ssh.elf $(BUILD)/sftp.elf
$(SUNSET)/src/config.rs: prepare-sunset.py Cargo.lock Makefile
	python3 prepare-sunset.py "$(RUSTUP)" "$(SUNSET)" "$(TARGET)"
$(LIB): $(SUNSET)/src/config.rs Cargo.toml Cargo.lock src/lib.rs src/sftp.rs Makefile
	env -u CARGO_ENCODED_RUSTFLAGS RUSTFLAGS='--cfg getrandom_backend="custom" --cfg sha2_backend="soft" --cfg aes_backend="soft"' $(RUSTUP) run stable cargo build --config 'paths=["$(abspath $(SUNSET))"]' --release --locked --target $(TARGET) --target-dir $(OUT)
$(BUILD)/ssh.elf $(BUILD)/sftp.elf: $(BUILD)/%.elf: $(LIB) port.c ../tcc/api.c ../tcc/lixin.h ../console.h ../lust/link.ld ../ls/link.ld Makefile
	$(CC) -DTRANSFER=$(if $(filter sftp,$*),1,0) -Os -ffunction-sections -fdata-sections -nostartfiles -Wl,--gc-sections,-T,../lust/link.ld port.c ../tcc/api.c $(LIB) -lc -lnosys -lgcc -o $(OUT)/$*.linked.elf
	aarch64-none-elf-objcopy -O binary $(OUT)/$*.linked.elf $(OUT)/$*.bin
	echo '.section .text.start,"ax"; .global _start; _start:; .incbin "$(OUT)/$*.bin"' | $(CC) -x assembler -nostdlib -Wl,-T,../ls/link.ld,-s,-z,max-page-size=4096,--build-id=none -o $@.tmp -
	test $$(wc -c < $@.tmp) -le 1048576
	mv $@.tmp $@
