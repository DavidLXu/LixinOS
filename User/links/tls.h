#pragma once
#include <stddef.h>

struct tls_connection;
struct tls_connection *tls_open(int fd, const char *name);
const char *tls_send(struct tls_connection *, const char *, size_t);
int tls_receive(struct tls_connection *, void *, size_t);
const char *tls_error(struct tls_connection *);
void tls_free(struct tls_connection *);
