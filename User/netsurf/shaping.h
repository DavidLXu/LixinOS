#pragma once
#include <stddef.h>
#include <stdint.h>

struct shaped_run { size_t bytes; int width; unsigned count; int rtl; };
struct shaped_glyph { const uint8_t *mask; int x,y,width,height; };
/* A successful call owns the transient run until the next call to ns_shape. */
struct shaped_run ns_shape(const char *text,size_t len,int px);
struct shaped_glyph ns_shaped_glyph(unsigned i);
size_t ns_shaped_caret(struct shaped_run run,int x,int *actual);
