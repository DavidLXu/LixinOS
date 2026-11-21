#pragma once
#include <stdint.h>
int ns_han_width(uint32_t code);
const uint8_t *ns_han_glyph(uint32_t code, int scale);
int ns_char_advance(uint32_t code, int style, int scale);
#include "../gui/han-font.h"
int ns_advance(unsigned code, int style, int px);
const unsigned char *ns_outline_mask(unsigned code, int style, int px, int *stride);
int ns_break_between(unsigned before, unsigned after);
