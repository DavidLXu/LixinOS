/* Recognize only the guest's supported ELF interpreter. The musl bootstrap
 * subsequently validates and maps its own segments in EL0. */
#include "native.h"
static u64 field(const u8 *p, unsigned n) {
    u64 value=0;
    for (unsigned i=0; i<n; ++i) value |= (u64)p[i] << (i*8);
    return value;
}
int musl_program(const u8 *p, u64 size) {
    static const char interp[] = "/lib/ld-musl-aarch64.so.1";
    if (size < 64 || memcmp(p,"\177ELF\2\1\1",7) || field(p+16,2) != 3 ||
        field(p+18,2) != 183 || field(p+20,4) != 1 || field(p+54,2) != 56) return 0;
    u64 ph = field(p+32,8), count = field(p+56,2);
    if (ph > size || count > (size-ph)/56) return 0;
    for (unsigned i=0; i<count; ++i) {
        const u8 *h = p+ph+i*56;
        if (field(h,4) != 3) continue;
        u64 off=field(h+8,8), n=field(h+32,8);
        return off <= size && n == sizeof(interp) && n <= size-off && !memcmp(p+off,interp,n);
    }
    return 0;
}
