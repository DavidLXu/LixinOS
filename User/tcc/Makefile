BUILD = ../../Kernel/target/user-build
TCC = $(BUILD)/tinycc
all: $(BUILD)/tcc.bin

$(BUILD)/tccdefs_.h: $(TCC)/include/tccdefs.h $(TCC)/conftest.c
	cc -DC2STR $(TCC)/conftest.c -o $(BUILD)/c2str
	$(BUILD)/c2str $< $@

$(BUILD)/tcc.bin: port.c platform.c api.c slots.def lixin.h $(BUILD)/lixin-header.inc $(BUILD)/gui-header.inc config.h ../console.h ../user.ld $(BUILD)/tccdefs_.h
	aarch64-none-elf-gcc -DLX_COMPILER -I. -I$(BUILD) -I$(TCC) -Os -ffunction-sections -fdata-sections -nostartfiles -Wl,--gc-sections,--defsym=__user_heap_limit=0x443d7000,-T,../user.ld port.c platform.c api.c -lm -lc -lnosys -lgcc -o $(BUILD)/tcc.elf
	aarch64-none-elf-objcopy -O binary $(BUILD)/tcc.elf $@

all: $(BUILD)/runner.bin $(BUILD)/lxe-runtime.elf
$(BUILD)/lxe-runtime.elf: $(BUILD)/runner.bin
	aarch64-none-elf-strip --strip-all -o $@ $(BUILD)/runner.elf

$(BUILD)/runner.bin: runner.c platform.c api.c slots.def lixin.h ../gui/lixin-gui.h $(BUILD)/lixin-header.inc $(BUILD)/gui-header.inc ../console.h ../user.ld
	aarch64-none-elf-gcc -DLX_SAVED_RUNTIME -I$(BUILD) -Os -ffunction-sections -fdata-sections -nostartfiles -Wl,--gc-sections,--defsym=__user_heap_limit=0x44300000,-T,../user.ld runner.c platform.c api.c -lm -lc -lnosys -lgcc -o $(BUILD)/runner.elf
	aarch64-none-elf-objcopy -O binary $(BUILD)/runner.elf $@

$(BUILD)/lixin-header.inc: lixin.h
	python3 -c 'import json,sys; print(json.dumps(open(sys.argv[1]).read()))' $< > $@

$(BUILD)/gui-header.inc: ../gui/lixin-gui.h
	python3 -c 'import json,sys; print(json.dumps(open(sys.argv[1]).read()))' $< > $@
