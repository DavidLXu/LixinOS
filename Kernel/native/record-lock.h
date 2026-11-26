#pragma once
/* Service 65. The descriptor is an open native regular file. Guest identifies
 * a Linux process within its native owner. Ranges use inclusive byte offsets. */
#define LX_LOCK_VERSION 0x4c584c4b
#define LX_LOCK_PROBE (~0UL)
#define LX_LOCK_LAST 0x7fffffffffffffffUL
enum lx_lock_type { LX_LOCK_READ, LX_LOCK_WRITE, LX_LOCK_UNLOCK };
struct lx_record_request {
    unsigned long first, last;
    unsigned guest, type;
};
long record_call(unsigned long *owner, const char *path, unsigned length, int writable,
                 unsigned query, struct lx_record_request *request);
void record_release(unsigned long owner);
