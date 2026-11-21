BUILD := ../../Kernel/target/user-build
.PHONY: all font
all: font
	$(MAKE) -C ../links $(BUILD)/desktop.elf
font: $(BUILD)/desktop-font.o
$(BUILD)/desktop-font.o: font.swift Makefile
	swift font.swift $(BUILD)/desktop.gray
	echo '.section .rodata; .global desktop_font; desktop_font:; .incbin "$(BUILD)/desktop.gray"' | aarch64-none-elf-gcc -x assembler -c -o $@ -

$(BUILD)/nxeyes.elf $(BUILD)/stopwatch.elf: $(BUILD)/%.elf: %.c window-client.h ../ls/link.ld ../console.h ../tcc/api.c ../tcc/lixin.h
	aarch64-none-elf-gcc -Os -ffreestanding -nostdlib -ffunction-sections -fdata-sections -Wl,--gc-sections,-T,../ls/link.ld,-s,-z,max-page-size=4096,--build-id=none $< ../tcc/api.c -o $@
