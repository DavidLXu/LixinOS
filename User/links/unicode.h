#ifndef LX_UNICODE_H
#define LX_UNICODE_H
/* Each wide character occupies a lead cell and an unpainted continuation. */
#define WIDE_TAIL 0x110000
static inline int han_slot(unsigned c) {
    if (c>=0x3000 && c<0xa000) return c-0x3000;
    if (c>=0xf900 && c<0xfb00) return 0x7000+c-0xf900;
    if (c>=0xff00 && c<0xff61) return 0x7200+c-0xff00;
    return -1;
}
static inline unsigned utf8_next(const char **text) {
    const unsigned char *s=(const unsigned char *)*text;
    unsigned c=*s++;
    int n=c<128?0:c>=0xf0?3:c>=0xe0?2:c>=0xc2?1:0;
    if (n) {
        c &= 0x7f >> (n+1);
        while (n--) {
            if ((*s&0xc0)!=0x80) { c=0xfffd; break; }
            c=(c<<6)|(*s++&63);
        }
    }
    *text=(const char *)s;
    return c;
}
#endif
