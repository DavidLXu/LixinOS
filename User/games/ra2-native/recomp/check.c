#include <stdio.h>
#include <string.h>
#include "win32hle.h"
int check_pe(void);
int check_draw(void);
int check_host(int argc,char **argv);
int main(int argc,char **argv) {
    if(argc==2 && strcmp(argv[1],"host")==0) {
        char *args[]={"ra2-recomp-check","--game","/lib/ra2-recomp-check","--headless","--mute","--run",NULL};
        return check_host(6,args);
    }
    if(argc==2 && strcmp(argv[1],"draw")==0) return check_draw();
    // Verify high guest addresses, zero/decommit and inverse translation.
    uint32_t addr=hle_vm_reserve(0xe1000000,131072);
    if(addr!=0xe1000000 || hle_vm_reserve(addr,65536)!=0) return 1;
    uint32_t *word=(uint32_t*)hle_ptr(addr+65540);
    *word=0xa55a;
    if(hle_va(word)!=addr+65540 || MEM32(addr+65540)!=0xa55a) return 1;
    hle_vm_zero(addr+65536,4096);
    if(*word) return 1;
    hle_vm_release(addr,131072);
    if(hle_ptr(addr)!=NULL) return 1;
    puts("Win32 sparse addresses verified");
    return check_pe();
}
