// The kernel maps the executable's two PT_LOAD segments. Supply the static
// TLS descriptor to musl and use the private process arena for stack and heap.
extern unsigned char lx_tls_phdr[], lx_musl_start[];
#define IMAGE_BASE 0x78000000UL
#define STACK_TOP (IMAGE_BASE + (2UL << 20))
#define PHDR_ADDR lx_tls_phdr
#define PHNUM 1
#define ENTRY_PC lx_musl_start
// The utility ABI supplies the arguments after the program name.
#define LX_ARGV0 ""
