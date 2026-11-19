#pragma once
// LixinOS byte-oriented foreground console ABI.
#ifdef __TINYC__
long lx_console_call(long operation, long value);
#define console_call lx_console_call
#else
static inline long console_call(long operation, long value) {
    register long x0 asm("x0") = value;
    register long x8 asm("x8") = operation;
    asm volatile("svc #0" : "+r"(x0) : "r"(x8) : "x9", "x10", "memory", "cc");
    return x0;
}
#endif
static inline int get_byte(void) { return console_call(0, 0); }
static inline void put_byte(char c) { console_call(1, (unsigned char)c); }
static inline __attribute__((noreturn)) void finish(int status) {
    console_call(2, status);
    for (;;) {}
}
