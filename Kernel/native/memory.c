#include "native.h"

int normal_ram;
typedef u64 unaligned_word __attribute__((aligned(1), may_alias));
void *memcpy(void *out, const void *in, u64 n) {
    u8 *d = out;
    const u8 *s = in;
    if (normal_ram || !(((u64)d | (u64)s) & 7)) {
        while (n >= 32) {
            ((unaligned_word *)d)[0] = ((const unaligned_word *)s)[0];
            ((unaligned_word *)d)[1] = ((const unaligned_word *)s)[1];
            ((unaligned_word *)d)[2] = ((const unaligned_word *)s)[2];
            ((unaligned_word *)d)[3] = ((const unaligned_word *)s)[3];
            d += 32;
            s += 32;
            n -= 32;
        }
    }
    while (n--)
        *d++ = *s++;
    return out;
}
void *memset(void *out, int c, u64 n) {
    u8 *d = out;
    u64 fill = (u8)c * 0x0101010101010101UL;
    while (n && ((u64)d & 7)) {
        *d++ = c;
        n--;
    }
    while (n >= 32) {
        u64 *w = (u64 *)d;
        w[0] = w[1] = w[2] = w[3] = fill;
        d += 32;
        n -= 32;
    }
    while (n--)
        *d++ = c;
    return out;
}

void *memmove(void *out, const void *in, u64 n) {
    u8 *dst = out;
    const u8 *src = in;
    if ((u64)out <= (u64)in)
        return memcpy(out, in, n);
    while (n) {
        --n;
        dst[n] = src[n];
    }
    return out;
}
u64 strlen(const char *s) {
    const char *end = s;
    while (*end)
        end++;
    return end - s;
}
int memcmp(const void *a, const void *b, u64 n) {
    const u8 *x = a, *y = b;
    while (n--) {
        if (*x != *y)
            return *x - *y;
        x++;
        y++;
    }
    return 0;
}
int strncmp(const char *a, const char *b, u64 n) {
    while (n--) {
        if (*a != *b)
            return (u8)*a - (u8)*b;
        if (!*a)
            break;
        a++;
        b++;
    }
    return 0;
}
char *add_text(char *out, const char *s) {
    while (*s)
        *out++ = *s++;
    *out = 0;
    return out;
}
char *add_number(char *out, u64 value) {
    char digits[20];
    unsigned n = 0;
    do {
        digits[n++] = '0' + value % 10;
        value /= 10;
    } while (value);
    while (n)
        *out++ = digits[--n];
    *out = 0;
    return out;
}

int atoi(const char *s) {
    while (*s == ' ' || (*s >= 9 && *s <= 13))
        s++;
    int sign = *s == '-' ? -1 : 1;
    if (*s == '-' || *s == '+')
        s++;
    unsigned result = 0;
    while (*s >= '0' && *s <= '9')
        result = result * 10 + *s++ - '0';
    return sign < 0 ? -result : result;
}
