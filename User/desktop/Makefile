BUILD := ../../Kernel/target/user-build
all: $(BUILD)/desktop.elf
$(BUILD)/desktop-font.o: font.swift Makefile
	swift font.swift $(BUILD)/desktop.gray
	echo '.section .rodata; .global desktop_font; desktop_font:; .incbin "$(BUILD)/desktop.gray"' | aarch64-none-elf-gcc -x assembler -c -o $@ -
$(BUILD)/desktop.elf: desktop.c Makefile $(BUILD)/desktop-font.o ../tcc/api.c ../tcc/lixin.h ../console.h ../ls/link.ld
	aarch64-none-elf-gcc -Os -ffunction-sections -fdata-sections -mgeneral-regs-only -nostartfiles \
	    -Wl,--gc-sections,-T,../ls/link.ld,-s,-z,max-page-size=4096,--build-id=none \
	    desktop.c ../tcc/api.c $(BUILD)/desktop-font.o -lc -lnosys -lgcc -o $@
