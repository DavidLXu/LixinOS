#include <cstdio>

extern "C" FILE *win3_export_open(const char *, const char *);
#define fopen win3_export_open
#include "lodepng.cpp"
