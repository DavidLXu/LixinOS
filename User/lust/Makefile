BUILD := ../../Kernel/target/user-build
PORT := $(BUILD)/lust-port
UP := $(BUILD)/lust-src/src
RUSTUP := $(or $(shell command -v rustup),$(HOME)/.cargo/bin/rustup)
CC := aarch64-none-elf-gcc
all: $(BUILD)/lust.elf

$(PORT)/lust.a: $(UP)/cgen.rs prepare.py runtime.h lib.rs Makefile
	python3 prepare.py $(UP) $(PORT)
	cp lib.rs $(PORT)/
	$(RUSTUP) run stable rustc --edition=2024 --crate-name lust --crate-type staticlib --target aarch64-unknown-none-softfloat -C panic=abort -C opt-level=z -C lto=fat $(PORT)/lib.rs -o $@

$(BUILD)/lust.elf: $(PORT)/lust.a driver.c link.ld ../ls/link.ld ../tcc/port.c ../tcc/platform.c ../tcc/api.c ../tcc/lixin.h ../tcc/config.h ../tcc/slots.def ../console.h $(BUILD)/lixin-header.inc $(BUILD)/tccdefs_.h Makefile
	$(CC) -DLX_COMPILER -DLX_LUST -I../tcc -I$(BUILD) -I$(BUILD)/tinycc -Os -ffunction-sections -fdata-sections -nostartfiles -Wl,--gc-sections,-T,link.ld driver.c ../tcc/port.c ../tcc/platform.c ../tcc/api.c $(PORT)/lust.a -lm -lc -lnosys -lgcc -o $(PORT)/linked.elf
	aarch64-none-elf-objcopy -O binary $(PORT)/linked.elf $(PORT)/image.bin
	echo '.section .text.start,"ax"; .global _start; _start:; .incbin "$(PORT)/image.bin"' | $(CC) -x assembler -nostdlib -Wl,-T,../ls/link.ld,-s,-z,max-page-size=4096,--build-id=none -o $@.tmp -
	test $$(wc -c < $@.tmp) -le 1048576
	mv $@.tmp $@

all: $(BUILD)/lixinos-rust-arm64.tar.gz
$(BUILD)/lixinos-rust-arm64.tar.gz: $(BUILD)/lust.elf hello.rs README.md LICENSE $(BUILD)/tinycc/COPYING
	mkdir -p $(PORT)/package/bin $(PORT)/package/src/lust $(PORT)/package/licenses
	cp $(BUILD)/lust.elf $(PORT)/package/bin/lust
	cp hello.rs $(PORT)/package/src/lust/
	cp README.md $(PORT)/package/
	cp LICENSE $(PORT)/package/licenses/lust-MIT
	cp $(BUILD)/tinycc/COPYING $(PORT)/package/licenses/tinycc-LGPL
	tar -czf $@.tmp -C $(PORT)/package bin src README.md licenses
	mv $@.tmp $@
