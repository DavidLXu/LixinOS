CACHE := ../../Kernel/target/user-build
LIB := $(CACHE)/libgit2
OUT := $(CACHE)/git-objects
CC := aarch64-none-elf-gcc
FLAGS := -std=gnu11 -Os -g -ffunction-sections -fdata-sections -D_GNU_SOURCE -DNO_MMAP -DNDEBUG -include include/lx_posix.h -Iinclude -I$(LIB)/deps/pcre -I$(LIB)/include -I$(LIB)/src/libgit2 -I$(LIB)/src/util -I$(LIB)/deps/zlib -I$(LIB)/deps/xdiff
# Build the local repository implementation; the port has no network transports.
CORE := $(filter-out %/transport.c %/remote.c %/fetch.c %/push.c %/clone.c %/proxy.c %/libgit2.c,$(wildcard $(LIB)/src/libgit2/*.c))
UTIL := $(filter-out %/net.c %/rand.c %/process.c ,$(wildcard $(LIB)/src/util/*.c))
SRC := $(CORE) $(UTIL) $(LIB)/src/util/allocators/failalloc.c $(LIB)/src/util/allocators/stdalloc.c $(filter-out %/pcreposix.c,$(wildcard $(LIB)/deps/pcre/*.c)) $(LIB)/src/util/hash/collisiondetect.c $(LIB)/src/util/hash/builtin.c $(wildcard $(LIB)/src/util/hash/sha1dc/*.c) $(wildcard $(LIB)/src/util/hash/rfc6234/*.c) $(wildcard $(LIB)/deps/xdiff/*.c) $(wildcard $(LIB)/deps/zlib/*.c)
OBJ := $(patsubst $(LIB)/%.c,$(OUT)/%.o,$(SRC))
all: $(CACHE)/git.bin
$(OUT)/deps/pcre/%.o: FLAGS := -Ipcre -DHAVE_CONFIG_H $(FLAGS)
$(OUT)/%.o: $(LIB)/%.c Makefile
	@mkdir -p $(@D)
	$(CC) $(FLAGS) -MMD -MP -c $< -o $@
$(OUT)/libgit.a: $(OBJ)
	aarch64-none-elf-ar rcs $@ $^
$(CACHE)/git.bin: $(OUT)/libgit.a cli.c platform.c platform.h ../console.h ../user.ld
	$(CC) $(FLAGS) -nostartfiles -Wl,--gc-sections,-T,../user.ld cli.c platform.c $< -lm -lc -lnosys -lgcc -o $(OUT)/git.elf
	aarch64-none-elf-objcopy -O binary $(OUT)/git.elf $@
-include $(OBJ:.o=.d)
