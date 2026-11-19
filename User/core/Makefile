BUILD := ../../Kernel/target/user-build
COMMANDS := echo cat grep wc pwd clear netinfo cpuinfo sysinfo uname head tail tee true false sleep date sort uniq find ps df stat tree du cmp diff nl cut tr sed seq basename dirname test printf
all: $(addprefix $(BUILD)/core-,$(addsuffix .elf,$(COMMANDS)))
$(BUILD)/core-%.elf: commands.c text.h discovery.h transform.h arguments.h ../file-time.h ../ls/link.ld ../console.h ../tcc/api.c ../tcc/lixin.h Makefile
	aarch64-none-elf-gcc -Os -ffreestanding -nostdlib -mgeneral-regs-only -ffunction-sections \
	    -Wl,-T,../ls/link.ld,-s,-z,max-page-size=4096,--build-id=none,--gc-sections \
	    -DCOMMAND='"$*"' -o $@ commands.c ../tcc/api.c
