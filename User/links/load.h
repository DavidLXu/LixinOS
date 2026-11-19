#pragma once
#include <stddef.h>

#define PAGE_LIMIT (10UL * 1024 * 1024)
#define ADDRESS_LIMIT 1024
struct page_source {
    char *bytes, *body, *headers;
    size_t length;
    int status;
};
/* On success the caller owns bytes. A redirect replaces address in place. */
const char *load_page(char address[ADDRESS_LIMIT], struct page_source *out);

const char *load_resource(char address[ADDRESS_LIMIT],const char *parent,struct page_source *out);
const char *fetch_same_origin(const char *base, const char *ref, struct page_source *out);
