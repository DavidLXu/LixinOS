#ifndef LIXIN_LWIP_CC_H
#define LIXIN_LWIP_CC_H
#undef BIG_ENDIAN
#undef LITTLE_ENDIAN
#include <sys/types.h>
#ifndef SSIZE_MAX
#define SSIZE_MAX 0x7fffffffffffffffL
#endif
#define LWIP_NO_INTTYPES_H 1
#define PACK_STRUCT_STRUCT __attribute__((packed))

#define LWIP_NO_CTYPE_H 1
#define LWIP_NO_UNISTD_H 1
#define LWIP_PLATFORM_DIAG(x) ((void)0)
void net_assert(const char *);
unsigned net_random(void);
#define LWIP_PLATFORM_ASSERT(x) net_assert(x)
#define LWIP_RAND() net_random()
#define U16_F "u"
#define U32_F "u"
#define S16_F "d"
#define S32_F "d"
#define X16_F "x"
#define X32_F "x"
#define X8_F "02x"
#define SZT_F "lu"
#endif
