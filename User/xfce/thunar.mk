sources := $(wildcard $(SRC)/thunar/*.c $(SRC)/thunarx/*.c)
all: $(addprefix $(OUT)/,$(notdir $(sources:.c=.o)))
vpath %.c $(SRC)/thunar $(SRC)/thunarx
$(OUT)/%.o: %.c $(firstword $(MAKEFILE_LIST)) $(dir $(firstword $(MAKEFILE_LIST)))thunar-config/config.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OUT)/thunar-action-manager.o: $(dir $(firstword $(MAKEFILE_LIST)))thunar-config/session-open.h
