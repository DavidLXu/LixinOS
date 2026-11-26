#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdlib.h>
#define PAGE 4096UL
#define BLOCK (2UL<<20)
static unsigned long swap_free(void) {
    FILE *f=fopen("/proc/meminfo","r");
    char buf[160]; unsigned long value=0;
    if (f) {
        while (fgets(buf,sizeof(buf),f))
            if (sscanf(buf,"SwapFree: %lu",&value)==1) break;
        fclose(f);
    }
    return value;
}
int main(void) {
    unsigned long before=swap_free();
    volatile uint64_t *p=mmap(NULL,BLOCK*16,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (!before || p==MAP_FAILED) return 1;
    unsigned long stride=PAGE/sizeof(*p);
    for (unsigned i=0;i<8192;++i) {
        if (p[i*stride] || p[i*stride+511]) return 2;
        p[i*stride]=0xabc123456700UL+i;
        p[i*stride+511]=~p[i*stride];
    }
    unsigned long during=swap_free();
    if (during>=before) return 3;
    if (mprotect((void *)p,BLOCK,PROT_NONE) || mprotect((void *)p,BLOCK,PROT_READ)) return 4;
    for (unsigned j=8192;j;--j) {
        unsigned i=j-1;
        uint64_t value=0xabc123456700UL+i;
        if (p[i*stride]!=value || p[i*stride+511]!=~value) return 5;
    }
    void *reset=mmap((void *)p,PAGE,PROT_READ|PROT_WRITE,MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (reset!=(void *)p || p[0] || p[511] || p[stride]!=0xabc123456701UL) return 7;
    int fd=open("/swap-input",O_RDONLY);
    if (fd<0 || mmap((void *)p,PAGE,PROT_READ,MAP_FIXED|MAP_PRIVATE,fd,0)!=(void *)p) return 8;
    close(fd);
    for (unsigned i=1;i<16;++i)
        if (p[i*BLOCK/sizeof(*p)]!=0xabc123456700UL+i*512) return 9;
    if (memcmp((const void *)p,"private file",12)) return 10;
    if (munmap((void *)p,16*BLOCK) || swap_free()!=before) return 6;
    printf("swap roundtrip OK %lu KiB paged out, 8192 pages checked\n",before-during);
    return 0;
}
