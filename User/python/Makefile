CACHE = ../../Kernel/target/user-build
BUILD = $(CACHE)/python-objects
include $(CACHE)/micropython/py/mkenv.mk
include $(TOP)/py/py.mk

CFLAGS += -I. -I$(TOP) -I$(BUILD) -std=gnu11 -Os -DNDEBUG -ffreestanding -fno-stack-protector -fdata-sections -ffunction-sections
SRC_C = port.c shared/readline/readline.c shared/runtime/pyexec.c shared/runtime/gchelper_generic.c shared/runtime/stdout_helpers.c shared/libc/printf.c
SRC_QSTR += port.c shared/readline/readline.c shared/runtime/pyexec.c
OBJ = $(PY_CORE_O) $(addprefix $(BUILD)/,$(SRC_C:.c=.o))

all: $(CACHE)/python.bin
$(CACHE)/python.bin: $(OBJ) ../user.ld
	$(CC) -nostartfiles -nostdlib -Wl,--gc-sections,-T,../user.ld $(OBJ) -lm -lc -lgcc -o $(CACHE)/python.elf
	$(OBJCOPY) -O binary $(CACHE)/python.elf $@

include $(TOP)/py/mkrules.mk

all: $(CACHE)/echo.bin
$(CACHE)/echo.bin: ../echo.c ../console.h ../user.ld
	$(CC) -Os -ffreestanding -nostdlib -T ../user.ld ../echo.c -o $(CACHE)/echo.elf
	$(OBJCOPY) -O binary $(CACHE)/echo.elf $@
