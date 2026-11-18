typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned uint32_t;
typedef long int64_t;
typedef unsigned long uint64_t;
#define INT64_MIN (-9223372036854775807L-1)
#include <stdio.h>
void *realloc(void *, unsigned long);
void free(void *);
void abort(void);
void *memcpy(void *, const void *, unsigned long);
void *memmove(void *, const void *, unsigned long);
int memcmp(const void *, const void *, unsigned long);
unsigned long strlen(const char *);
int fputs(const char *, FILE *);
int fflush(FILE *);
int snprintf(char *, unsigned long, const char *, ...);
unsigned long fwrite(const void *, unsigned long, unsigned long, FILE *);
