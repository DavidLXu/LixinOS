// Build inside LixinOS: tcc /src/net/tcpcheck.c -o /bin/tcpcheck
#include <lixin.h>
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc != 3 || !argv[2][0]) {
        puts("usage: tcpcheck IPv4 PORT");
        return 1;
    }
    int port = 0;
    for (const char *p = argv[2]; *p; ++p) {
        if (*p < '0' || *p > '9' || (port = port * 10 + (*p - '0')) > 65535) {
            puts("invalid TCP port");
            return 1;
        }
    }
    if (!port) {
        puts("invalid TCP port");
        return 1;
    }
    int fd = lx_tcp_connect(argv[1], port);
    if (fd < 0) {
        puts("TCP connection failed");
        return 1;
    }
    printf("TCP connected to %s:%d\n", argv[1], port);
    return lx_tcp_close(fd) < 0;
}
