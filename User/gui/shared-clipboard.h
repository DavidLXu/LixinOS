#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>

#define LX_CLIP_MAX (1024U * 1024)
#define LX_CLIP_FILE "/tmp/.lixinos-clipboard"
static int clip_transfer(int fd, void *buf, size_t size, int writing) {
    unsigned char *p = (unsigned char *)buf;
    while (size) {
        ssize_t n = writing ? write(fd, p, size) : read(fd, p, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        size -= n;
        p += n;
    }
    return 0;
}
/* Caller owns *data after a successful paste (including an empty selection). */
static int clip_mac(int copy, unsigned char **data, unsigned *size) {
    if (*size > LX_CLIP_MAX)
        return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8767);
    inet_pton(AF_INET, "10.0.2.104", &addr.sin_addr);
    unsigned char header[5] = {(unsigned char)(copy ? 'C' : 'P')};
    uint32_t length = htonl(*size);
    memcpy(header + 1, &length, 4);
    int ok = connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0 && clip_transfer(fd, header, 5, 1) == 0 &&
             (!copy || clip_transfer(fd, *data, *size, 1) == 0) && clip_transfer(fd, header, 5, 0) == 0 &&
             header[0] == 'K';
    if (ok && !copy) {
        memcpy(&length, header + 1, 4);
        length = ntohl(length);
        unsigned char *result = length <= LX_CLIP_MAX ? (unsigned char *)malloc(length + 1) : NULL;
        ok = result && clip_transfer(fd, result, length, 0) == 0;
        if (ok) {
            result[length] = 0;
            *data = result;
            *size = length;
        } else
            free(result);
    }
    close(fd);
    return ok ? 0 : -1;
}
static int clip_store(const void *data, unsigned size) {
    char name[] = LX_CLIP_FILE ".XXXXXX";
    if (size > LX_CLIP_MAX)
        return -1;
    int fd = mkstemp(name);
    if (fd < 0)
        return -1;
    int status = clip_transfer(fd, (void *)data, size, 1);
    if (close(fd) < 0 || status < 0 || rename(name, LX_CLIP_FILE) < 0) {
        unlink(name);
        return -1;
    }
    return 0;
}
