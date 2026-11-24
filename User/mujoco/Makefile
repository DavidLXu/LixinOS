CC := $(MUSL)/cc
CXXINC := $(ALPINE)/include/c++/14.2.0
FLAGS := -Os -ffunction-sections -fdata-sections -D_GNU_SOURCE -DCCD_STATIC_DEFINE -I$(SRC)/include -I$(SRC)/src -I$(CCD)/src -I$(BUILD)/include
CXXFLAGS := -std=c++20 -isystem $(CXXINC) -isystem $(CXXINC)/aarch64-alpine-linux-musl -idirafter $(ALPINE)/include
VPATH := $(SRC)/src/engine:$(CCD)/src:$(PORT)
sources := $(wildcard $(SRC)/src/engine/*.c $(SRC)/src/engine/*.cc $(CCD)/src/*.c)
objects := $(addprefix $(BUILD)/obj/,$(addsuffix .o,$(basename $(notdir $(sources)))))
$(BUILD)/obj/%.o: %.c
	$(CC) $(FLAGS) -std=gnu11 -MMD -MP -c $< -o $@
$(BUILD)/obj/%.o: %.cc
	$(CC) $(FLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/crt0.o: $(PORT)/../cpython/loader.c
	$(CC) -Os -ffreestanding -fno-builtin -DLX_APPLICATION -I$(PORT)/../gcc -c $< -o $@
$(BUILD)/crt1.o: $(MUSL)/sysroot/lib/crt1.o
	aarch64-none-elf-objcopy --redefine-sym _start=lx_musl_start $< $@
$(BUILD)/libmujoco-engine.a: $(objects)
	aarch64-none-elf-ar rcs $@ $^
$(BUILD)/mujoco: $(BUILD)/crt0.o $(BUILD)/crt1.o $(BUILD)/obj/run.o $(BUILD)/libmujoco-engine.a
	$(CC) -nostartfiles -static -Wl,--gc-sections,--build-id=none,-z,max-page-size=4096 -Wl,-T,$(PORT)/../gcc/application.ld -o $@ $(shell aarch64-none-elf-gcc -print-file-name=crtbegin.o) $^ -Wl,--start-group $(RT) $(ALPINE)/lib/libstdc++.a -lm -lc -lgcc -Wl,--end-group $(shell aarch64-none-elf-gcc -print-file-name=crtend.o)
-include $(objects:.o=.d) $(BUILD)/obj/run.d
