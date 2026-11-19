#include <bearssl.h>
#include <stdlib.h>
#include <stdio.h>
#include "anchors.h"
#include "tls.h"
#include "../console.h"
#include "../tcc/lixin.h"

struct tls_connection {
    br_ssl_client_context client;
    br_x509_minimal_context cert;
    br_sslio_context io;
    unsigned char records[BR_SSL_BUFSIZE_BIDI];
    int fd;
};

static int transfer(int fd, void *buffer, size_t length, int writing) {
    for (int i = 0; i < 1500; i++) {
        int n = writing ? lx_tcp_write(fd, buffer, length) : lx_tcp_read(fd, buffer, length);
        if (n != LX_TCP_AGAIN) return n > 0 ? n : -1;
        if (lx_timed_byte(8) == 3) break;
    }
    return -1;
}
static int receive_record(void *ctx, unsigned char *data, size_t len) {
    return transfer(*(int *)ctx, data, len, 0);
}
static int send_record(void *ctx, const unsigned char *data, size_t len) {
    return transfer(*(int *)ctx, (void *)data, len, 1);
}
struct tls_connection *tls_open(int fd, const char *name) {
    unsigned char seed[32];
    unsigned long now = console_call(36, 0);
    if (now > 0xffffffffUL || !now || lx_random(seed, sizeof(seed)) < 0) return NULL;
    struct tls_connection *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->fd = fd;
    br_ssl_client_init_full(&c->client, &c->cert, TAs, TAs_NUM);
    br_ssl_engine_set_versions(&c->client.eng, BR_TLS12, BR_TLS12);
    const uint16_t suites[] = {
        BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
        BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
        BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
        BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
        BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
        BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384
    };
    br_ssl_engine_set_suites(&c->client.eng, suites, sizeof(suites)/sizeof(*suites));
    br_x509_minimal_set_time(&c->cert, 719528 + now / 86400, now % 86400);
    br_ssl_engine_set_buffer(&c->client.eng, c->records, sizeof(c->records), 1);
    br_ssl_engine_inject_entropy(&c->client.eng, seed, sizeof(seed));
    if (!br_ssl_client_reset(&c->client, name, 0)) { free(c); return NULL; }
    br_sslio_init(&c->io, &c->client.eng, receive_record, &c->fd, send_record, &c->fd);
    return c;
}
const char *tls_error(struct tls_connection *c) {
    switch (br_ssl_engine_last_error(&c->client.eng)) {
    case BR_ERR_X509_NOT_TRUSTED: return "TLS certificate is not trusted";
    case BR_ERR_X509_BAD_SERVER_NAME: return "TLS certificate hostname mismatch";
    case BR_ERR_X509_EXPIRED: return "TLS certificate outside validity period";
    default: { static char message[80]; snprintf(message,sizeof(message),"TLS connection failed (BearSSL %d)",br_ssl_engine_last_error(&c->client.eng)); return message; }
    }
}
const char *tls_send(struct tls_connection *c, const char *data, size_t len) {
    if (br_sslio_write_all(&c->io, data, len) || br_sslio_flush(&c->io)) return tls_error(c);
    return NULL;
}
int tls_receive(struct tls_connection *c, void *data, size_t len) {
    int n = br_sslio_read(&c->io, data, len);
    if (n < 0 && !br_ssl_engine_last_error(&c->client.eng)) return 0;
    return n;
}
void tls_free(struct tls_connection *c) { free(c); }
