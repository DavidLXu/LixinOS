#include "links.h"
#include "../console.h"
#include "../tcc/lixin.h"
void *_sbrk(ptrdiff_t n) {
    static size_t used;
    if (n < 0 || (size_t)n > 128UL*1024*1024-used) { errno=ENOMEM; return (void *)-1; }
    long base=console_call(37,used+n);
    if (base < 0) { errno=ENOMEM; return (void *)-1; }
    void *old=(char *)base+used;
    used+=n;
    return old;
}
void fatal_exit(const char *message, ...) {
    char buf[256];
    va_list args;
    va_start(args,message);
    vsnprintf(buf,sizeof(buf),message,args);
    va_end(args);
    lx_error("links: "); lx_error(buf); lx_error("\r\n"); finish(1);
}
int errline, bookmarks_codepage, page_size=4096, force_html;
unsigned char *errfile;
void int_error(const char *message, ...) {
    fatal_exit("renderer error at %s:%d", errfile,errline);
}
static void *checked(void *p, int mayfail) {
    if (!p && !mayfail) fatal_exit("page exceeds available browser memory");
    return p;
}
void *mem_alloc_(size_t n,int mayfail) { return n ? checked(malloc(n),mayfail) : DUMMY; }
void *mem_calloc_(size_t n,int mayfail) { return n ? checked(calloc(1,n),mayfail) : DUMMY; }
void mem_free(void *p) { if (p != DUMMY) free(p); }
void *mem_realloc_(void *p,size_t n,int mayfail) {
    if (!n) {mem_free(p); return DUMMY;}
    return checked(realloc(p == DUMMY ? NULL : p,n),mayfail);
}
// The host supplies synchronous documents. No request owns a network job.
void release_object(struct object_request **rq) { *rq=NULL; }
void selected_item(struct terminal *term,void *item,void *session) {}
int known_image_type(unsigned char *type) { return !casecmp(type,(unsigned char *)"image/",6); }
int dmp = 1;
