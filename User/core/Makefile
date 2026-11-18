BUILD := ../../Kernel/target/user-build
COMMANDS := echo cat grep wc pwd clear netinfo
all: $(addprefix $(BUILD)/core-,$(addsuffix .elf,$(COMMANDS)))
$(BUILD)/core-%.elf: commands.c ../ls/link.ld ../console.h ../tcc/api.c ../tcc/lixin.h Makefile
	aarch64-none-elf-gcc -Os -ffreestanding -nostdlib -mgeneral-regs-only -ffunction-sections \
	    -Wl,-T,../ls/link.ld,-s,-z,max-page-size=4096,--build-id=none,--gc-sections \
	    -DCOMMAND='"$*"' -o $@ commands.c ../tcc/api.c
