BUILD := ../../Kernel/target/user-build
SRC := $(BUILD)/links-2.30
OUT := $(BUILD)/links-port
CC := aarch64-none-elf-gcc
WEB := $(BUILD)/links-web
BEAR := $(BUILD)/bearssl-0.6
CA_BUNDLE := $(firstword $(wildcard /opt/homebrew/etc/ca-certificates/cert.pem /etc/ssl/cert.pem))
CFLAGS := -I$(WEB) -I$(BEAR)/inc -std=gnu11 -Os -fno-strict-aliasing -ffunction-sections -fdata-sections -DHAVE_CONFIG_H -I$(OUT) -I../git/include -I. -I$(SRC)
ENGINE := html html_r html_tbl charsets string memory view jsint types url headers helpers
OBJS := $(addprefix $(OUT)/,$(addsuffix .o,$(ENGINE)))
all: $(BUILD)/links.elf
$(OUT)/prepared: unicode.h prepare.py $(SRC)/links.h
	python3 prepare.py $(SRC) $(OUT)
	touch $@
$(OBJS): $(OUT)/%.o: $(OUT)/prepared config.h Makefile
	$(CC) $(CFLAGS) -c $(OUT)/$*.c -o $@
$(OUT)/anchors.h: $(CA_BUNDLE) $(BEAR)/build/brssl | $(OUT)/prepared
	$(BEAR)/build/brssl ta $(CA_BUNDLE) > $@.tmp
	mv $@.tmp $@
$(WEB)/ready: webdeps.py
	python3 webdeps.py $(WEB)
	touch $@
$(OUT)/duktape.o: $(WEB)/ready | $(OUT)/prepared
	$(CC) -Os -std=gnu11 -ffunction-sections -fdata-sections -c $(WEB)/duktape.c -o $@
$(OUT)/events.inc: events.js | $(OUT)/prepared
	python3 -c 'import json,sys; print(json.dumps(open(sys.argv[1]).read()))' $< > $@
$(BUILD)/links.elf: $(OUT)/events.inc $(OUT)/duktape.o $(OUT)/anchors.h $(BEAR)/arm/libbearssl.a cookies.c cookies.h page.c page.h image.c unicode.h tls.c tls.h window.c window.h ../gui/lixin-gui.h $(BUILD)/desktop-font.o $(OBJS) load.h port.c load.c platform.c ../tcc/api.c ../tcc/lixin.h ../console.h ../lust/link.ld ../ls/link.ld
	$(CC) $(CFLAGS) -nostartfiles -Wl,--gc-sections,-T,../lust/link.ld port.c load.c cookies.c page.c image.c tls.c window.c $(BUILD)/desktop-font.o platform.c ../tcc/api.c $(OBJS) $(OUT)/duktape.o $(BEAR)/arm/libbearssl.a -lc -lm -lnosys -lgcc -o $(OUT)/links.elf
	aarch64-none-elf-objcopy -O binary $(OUT)/links.elf $(OUT)/image.bin
	echo '.section .text.start,"ax"; .global _start; _start:; .incbin "$(OUT)/image.bin"' | $(CC) -x assembler -nostdlib -Wl,-T,../ls/link.ld,-s,-z,max-page-size=4096,--build-id=none -o $@.tmp -
	test $$(wc -c < $@.tmp) -le 1048576
	mv $@.tmp $@
