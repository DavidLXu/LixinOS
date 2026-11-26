// Back each reserved Win32 region with a contiguous LixinOS allocation.
// Guest page-table entries cover 64 KiB, matching VirtualAlloc granularity.
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <pthread.h>

#define PAGE_BITS 16
#define PAGE_SIZE (1u << PAGE_BITS)
struct region {
    uint32_t base;
    size_t size;
    unsigned char *data;
    struct region *next;
};
static _Atomic(unsigned char *) pages[1u << (32-PAGE_BITS)];
static struct region *regions;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;

uintptr_t lixin_guest_ptr(uint32_t address) {
    unsigned char *page=atomic_load(&pages[address>>PAGE_BITS]);
    return page?(uintptr_t)(page+(address & (PAGE_SIZE-1))):0;
}
uint32_t lixin_guest_va(const void *p) {
    uintptr_t addr=(uintptr_t)p;
    uint32_t result=0;
    pthread_mutex_lock(&lock);
    for(struct region *r=regions;r;r=r->next) {
        uintptr_t start=(uintptr_t)r->data;
        if(addr>=start && addr-start<r->size) {result=r->base+(uint32_t)(addr-start);break;}
    }
    pthread_mutex_unlock(&lock);
    return result;
}
// Upstream serializes reserve/release under g_mem_lock and checks for overlap.
int lixin_guest_map(uint32_t address,uint64_t size,int prot) {
    if(!address || !size || (address & (PAGE_SIZE-1)) || (size & (PAGE_SIZE-1)) ||
       size>(1ull<<32)-address) return -1;
    pthread_mutex_lock(&lock);
    struct region *r;
    if(prot) {
        r=malloc(sizeof(*r));
        if(!r) goto fail;
        r->data=calloc(1,size);
        if(!r->data) {free(r);goto fail;}
        r->base=address;r->size=size;r->next=regions;regions=r;
    } else {
        struct region **link=&regions;
        while(*link && (*link)->base!=address) link=&(*link)->next;
        if(!*link || (*link)->size!=size) goto fail;
        r=*link;*link=r->next;
    }
    for(size_t offset=0;offset<size;offset+=PAGE_SIZE)
        atomic_store(&pages[(address+offset)>>PAGE_BITS],prot?r->data+offset:NULL);
    if(!prot) {free(r->data);free(r);}
    pthread_mutex_unlock(&lock);
    return 0;
fail:
    pthread_mutex_unlock(&lock);
    return -1;
}
