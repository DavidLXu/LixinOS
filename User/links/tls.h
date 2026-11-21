#pragma once
#include <stddef.h>

/* Return nonzero when the foreground user cancels a pending transfer. */
#ifdef LX_NETSURF
int lx_transport_wait(void);
#else
#include "../tcc/lixin.h"
static inline int lx_transport_wait(void) { return lx_timed_byte(8) == 3; }
#endif

struct tls_connection;
struct tls_connection *tls_open(int fd, const char *name);
const char *tls_send(struct tls_connection *, const char *, size_t);
int tls_receive(struct tls_connection *, void *, size_t);
const char *tls_error(struct tls_connection *);
void tls_free(struct tls_connection *);
