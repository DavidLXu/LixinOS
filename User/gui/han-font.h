#pragma once
unsigned char *lx_load_font_atlas(const char *path, int bytes);
/* A 16 by 16 glyph, two bytes per row, most significant bit first. */
const unsigned char *lx_han_glyph(unsigned codepoint);
/* 8-bit coverage at 16px or 32px. Valid until the next glyph lookup. */
const unsigned char *lx_pingfang_glyph(unsigned codepoint, int scale);
/* Returns the advance, or zero when absent. Optional alpha mask has a
 * 20*scale stride, 16*scale rows and a two-pixel (scaled) left margin. */
int lx_latin_glyph(unsigned codepoint, int style, int scale, const unsigned char **mask);
