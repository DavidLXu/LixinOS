#include "native.h"

void *memcpy(void *out, const void *in, u64 n) {
    u8 *dst = out;
    const u8 *src = in;
    while (n--)
        *dst++ = *src++;
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
void *memset(void *out, int c, u64 n) {
    u8 *dst = out;
    while (n--)
        *dst++ = c;
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
