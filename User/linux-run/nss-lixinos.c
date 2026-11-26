// glibc NSS service backed by the guest LixinOS network stack.
#include "resolver.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <nss.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

enum nss_status _nss_lixinos_gethostbyname2_r(const char *name, int family, struct hostent *result,
                                              char *buffer, size_t length, int *err, int *herr) {
    if (family != AF_INET) {
        *err = EAFNOSUPPORT;
        *herr = NO_DATA;
        return NSS_STATUS_NOTFOUND;
    }
    uintptr_t offset = -(uintptr_t)buffer & (sizeof(char *) - 1);
    size_t namesize = strlen(name) + 1;
    size_t header = offset + sizeof(char *) * 3 + sizeof(struct in_addr);
    if (length < header || length - header < namesize) {
        *err = ERANGE;
        *herr = NETDB_INTERNAL;
        return NSS_STATUS_TRYAGAIN;
    }
    char **slots = (char **)(buffer + offset);
    char *address = (char *)(slots + 3);
    if (syscall(LX_RESOLVE_IPV4, name, address) < 0) {
        *err = errno;
        *herr = HOST_NOT_FOUND;
        return NSS_STATUS_NOTFOUND;
    }
    slots[0] = NULL;
    slots[1] = address;
    slots[2] = NULL;
    result->h_aliases = slots;
    result->h_addr_list = slots + 1;
    result->h_addrtype = AF_INET;
    result->h_length = sizeof(struct in_addr);
    result->h_name = memcpy(buffer + header, name, namesize);
    *err = 0;
    *herr = NETDB_SUCCESS;
    return NSS_STATUS_SUCCESS;
}

enum nss_status _nss_lixinos_gethostbyname_r(const char *name, struct hostent *result, char *buffer,
                                             size_t length, int *err, int *herr) {
    return _nss_lixinos_gethostbyname2_r(name, AF_INET, result, buffer, length, err, herr);
}
