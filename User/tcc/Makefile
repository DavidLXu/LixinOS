BUILD = ../../Kernel/target/user-build
TCC = $(BUILD)/tinycc
all: $(BUILD)/tcc.bin

$(BUILD)/tccdefs_.h: $(TCC)/include/tccdefs.h $(TCC)/conftest.c
	cc -DC2STR $(TCC)/conftest.c -o $(BUILD)/c2str
	$(BUILD)/c2str $< $@

$(BUILD)/tcc.bin: port.c platform.c config.h ../console.h ../user.ld $(BUILD)/tccdefs_.h
	aarch64-none-elf-gcc -I. -I$(BUILD) -I$(TCC) -Os -ffunction-sections -fdata-sections -nostartfiles -Wl,--gc-sections,-T,../user.ld port.c platform.c -lm -lc -lnosys -lgcc -o $(BUILD)/tcc.elf
	aarch64-none-elf-objcopy -O binary $(BUILD)/tcc.elf $@

all: $(BUILD)/runner.bin
$(BUILD)/runner.bin: runner.c platform.c ../console.h ../user.ld
	aarch64-none-elf-gcc -Os -ffunction-sections -fdata-sections -nostartfiles -Wl,--gc-sections,--defsym=__user_heap_limit=0x44300000,-T,../user.ld runner.c platform.c -lc -lnosys -lgcc -o $(BUILD)/runner.elf
	aarch64-none-elf-objcopy -O binary $(BUILD)/runner.elf $@
