#pragma once
#include <stddef.h>
/* Private to this browser process. Domain attributes never widen the host scope. */
void cookie_store(const char *url,const char *line,size_t length,int from_http);
size_t cookie_list(const char *url,char *out,size_t capacity,int for_http);
