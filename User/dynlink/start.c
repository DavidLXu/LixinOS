#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define LIBC "/lib/ld-musl-aarch64.so.1"
#define PAGE 4096UL
#define STACK (2UL << 20)
extern char **environ;

static void fail(const char *reason) {
    fprintf(stderr,"ld-musl: %s\n", reason);
    _exit(126);
}
static void read_at(int fd, void *dest, size_t length, size_t offset) {
    char *out = dest;
    while (length) {
        ssize_t got = pread(fd,out,length,offset);
        if (got <= 0) fail("incomplete ELF interpreter");
        length -= got;
        offset += got;
        out += got;
    }
}
static void *load(Elf64_Ehdr *eh) {
    int fd = open(LIBC,O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd,&st)) fail("cannot open " LIBC);
    read_at(fd,eh,sizeof(*eh),0);
    if (memcmp(eh->e_ident,"\177ELF\2\1\1",7) || eh->e_machine != EM_AARCH64 ||
        eh->e_type != ET_DYN || !eh->e_phnum || eh->e_phnum > 32 ||
        eh->e_phentsize != sizeof(Elf64_Phdr) || eh->e_phoff > (size_t)st.st_size)
        fail("invalid interpreter header");
    Elf64_Phdr headers[32];
    read_at(fd,headers,eh->e_phnum * sizeof(*headers),eh->e_phoff);
    size_t span = 0;
    int entry_valid = 0, headers_valid = 0;
    for (unsigned i=0; i<eh->e_phnum; ++i) {
        const Elf64_Phdr *p = &headers[i];
        if (p->p_type != PT_LOAD) continue;
        if (p->p_vaddr >= (1UL<<30) || p->p_memsz > (1UL<<30)-p->p_vaddr ||
            p->p_filesz > p->p_memsz || p->p_offset > (size_t)st.st_size ||
            p->p_filesz > (size_t)st.st_size-p->p_offset || (p->p_flags & (PF_W|PF_X)) == (PF_W|PF_X))
            fail("invalid interpreter segment");
        size_t end = p->p_vaddr+p->p_memsz;
        if (end>span) span=end;
        if ((p->p_flags&PF_X) && eh->e_entry >= p->p_vaddr && eh->e_entry-p->p_vaddr < p->p_filesz) entry_valid=1;
        if (!p->p_vaddr && !p->p_offset && eh->e_phoff+eh->e_phnum*sizeof(*headers)<=p->p_filesz)
            headers_valid=1;
    }
    if (!entry_valid || !headers_valid) fail("interpreter has no startup code or headers");
    char *base=mmap(NULL,span,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (base == MAP_FAILED) fail("cannot reserve interpreter memory");
    for (unsigned i=0; i<eh->e_phnum; ++i) {
        const Elf64_Phdr *p = &headers[i];
        if (p->p_type != PT_LOAD || !p->p_memsz) continue;
        size_t start = p->p_vaddr & ~(PAGE-1);
        size_t length = p->p_vaddr-start+p->p_memsz;
        if (mprotect(base+start,length,PROT_READ|PROT_WRITE)) fail("cannot populate interpreter");
        read_at(fd,base+p->p_vaddr,p->p_filesz,p->p_offset);
        int prot = (p->p_flags & PF_R ? PROT_READ : 0) | (p->p_flags & PF_W ? PROT_WRITE : 0) |
                   (p->p_flags & PF_X ? PROT_EXEC : 0);
        if (mprotect(base+start,length,prot)) fail("cannot apply interpreter permissions");
    }
    close(fd);
    return base;
}
int main(int argc, char **argv) {
    Elf64_Ehdr eh;
    char *base = load(&eh);
    int direct = strcmp(argv[0],"/bin/ld-musl") != 0;
    int pip = !strcmp(argv[0],"/bin/pip") || !strcmp(argv[0],"/bin/pip3");
    if (!direct && argc < 2) fail("usage: ld-musl PROGRAM [ARG ...]");
    const char *program = pip ? "/bin/python3" : argv[!direct];
    char *mem=mmap(NULL,STACK+PAGE,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (mem==MAP_FAILED || mprotect(mem+PAGE,STACK,PROT_READ|PROT_WRITE)) fail("cannot allocate program stack");
    unsigned envc=0;
    while (environ[envc]) ++envc;
    uintptr_t *sp=(void *)(((uintptr_t)(mem+STACK+PAGE)-(argc+envc+40)*sizeof(uintptr_t)) & ~(uintptr_t)15);
    uintptr_t *v=sp;
    *v++=argc+direct+2*pip;
    *v++=(uintptr_t)LIBC;
    *v++=(uintptr_t)program;
    if (pip) {
        *v++=(uintptr_t)"-m";
        *v++=(uintptr_t)"pip";
    }
    for (int i=1+!direct; i<argc; ++i) *v++=(uintptr_t)argv[i];
    *v++=0;
    for (unsigned i=0; i<envc; ++i) *v++=(uintptr_t)environ[i];
    *v++=0;
    const uintptr_t aux[] = {AT_PHDR,(uintptr_t)base+eh.e_phoff,AT_PHNUM,eh.e_phnum,
        AT_PHENT,sizeof(Elf64_Phdr),AT_PAGESZ,PAGE,AT_BASE,(uintptr_t)base,
        AT_ENTRY,(uintptr_t)base+eh.e_entry,AT_RANDOM,getauxval(AT_RANDOM),
        0x6c780001UL,(uintptr_t)(mem+STACK+PAGE),AT_UID,getuid(),AT_EUID,geteuid(),AT_GID,getgid(),AT_EGID,getegid(),
        AT_EXECFN,(uintptr_t)program,AT_HWCAP,getauxval(AT_HWCAP),AT_NULL,0};
    memcpy(v,aux,sizeof(aux));
    __asm__ volatile("mov sp,%0\nbr %1" :: "r"(sp), "r"(base+eh.e_entry) : "memory");
    __builtin_unreachable();
}
