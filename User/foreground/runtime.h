#pragma once
#include <stddef.h>
#include <stdint.h>
unsigned long screen_info(void);
int read_timed(unsigned milliseconds);
void screen_write(const char *bytes, size_t length);
int save_file(const char *bytes, size_t length);
void kilo(const char *name, char *text, size_t len);
void nano(const char *name, const char *text, size_t len);
void htop(void);
