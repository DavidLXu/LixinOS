// Build upstream TinyCC as one translation unit so the freestanding loader
// can reuse its ELF symbol and relocation code without a POSIX dynamic loader.
#define ONE_SOURCE 1
#include "tcc.h"
#undef TCC_IS_NATIVE
#include "libtcc.c"
#include "../console.h"

#define CODE_BASE 0x44180000UL
static unsigned char machine_code[256*1024];

static void diagnostic(void *path, const char *message) {
    printf("%s: %s\n", (char *)path, message);
}

#define DATA_BASE 0x44300000UL
#define RESULT ((unsigned char *)0x443d8000UL)
#define MAX_FILE 16384
// LXE1: magic, entry offset, code length, data length (little-endian u32),
// then relocated code and initialized data, including zero-filled BSS.
static int link_program(TCCState *s, int save) {
    size_t used = 0, data_size = 0;
    resolve_common_syms(s);
    build_got_entries(s, 0);
    for (int i = 1; i < s->nb_sections; ++i) {
        Section *sec = s->sections[i];
        if (!(sec->sh_flags & SHF_ALLOC)) continue;
        if (sec->sh_flags & SHF_EXECINSTR) {
            used = (used + sec->sh_addralign - 1) & -(size_t)sec->sh_addralign;
            if (used > sizeof(machine_code) || sec->data_offset > sizeof(machine_code)-used) {
                puts("tcc: code region full");
                return 1;
            }
            sec->sh_addr = CODE_BASE + used;
            used += sec->data_offset;
        } else if (save) {
            size_t align = sec->sh_addralign ? sec->sh_addralign : 1;
            data_size = (data_size + align - 1) & -align;
            if (data_size > MAX_FILE || sec->data_offset > MAX_FILE - data_size) {
                puts("tcc: executable exceeds file limit"); return 1;
            }
            sec->sh_addr = DATA_BASE + data_size;
            data_size += sec->data_offset;
        } else {
            size_t align = sec->sh_addralign ? sec->sh_addralign : 1;
            void *storage = calloc(1, sec->data_offset + align);
            if (!storage) { puts("tcc: insufficient data memory"); return 1; }
            sec->sh_addr = ((uintptr_t)storage + align - 1) & -align;
        }
    }
    relocate_syms(s, s->symtab, 1);
    if (s->nb_errors) return 1;
    relocate_plt(s);
    relocate_sections(s);
    if (s->nb_errors) return 1;
    addr_t entry = get_sym_addr(s, "main", 1, 1);
    if (entry < CODE_BASE || entry >= CODE_BASE + used) { puts("tcc: missing main function"); return 1; }
    if (save) {
        if (16 + used + data_size > MAX_FILE) {puts("tcc: executable exceeds file limit"); return 1;}
        memset(RESULT, 0, 16 + used + data_size);
        memcpy(RESULT, "LXE1", 4);
        uint32_t header[] = {entry - CODE_BASE, used, data_size};
        memcpy(RESULT + 4, header, sizeof(header));
    }
    for (int i = 1; i < s->nb_sections; ++i) {
        Section *sec = s->sections[i];
        if (!(sec->sh_flags & SHF_ALLOC) || !sec->data || !sec->data_offset) continue;
        void *dest;
        if (sec->sh_flags & SHF_EXECINSTR)
            dest = (save ? RESULT + 16 : machine_code) + (sec->sh_addr - CODE_BASE);
        else if (save)
            dest = RESULT + 16 + used + (sec->sh_addr - DATA_BASE);
        else
            dest = (void *)sec->sh_addr;
        memcpy(dest, sec->data, sec->data_offset);
    }
    if (save) return 0;
    if (console_call(4, (long)machine_code)) return 1;
    int result = ((int (*)(void))entry)();
    printf("[exit %d]\n", result);
    return 0;
}

__attribute__((section(".text.start"), noreturn))
void _start(const char *path, const char *source, size_t len, long save) {
    (void)len;
    setbuf(stdout, NULL);
    TCCState *s = tcc_new();
    tcc_set_error_func(s, (void *)path, diagnostic);
    tcc_set_options(s, "-nostdlib -nostdinc");
    tcc_set_output_type(s, TCC_OUTPUT_MEMORY);
    tcc_add_include_path(s, "/include");
    tcc_add_symbol(s, "puts", save ? (void *)0x44170000 : (void *)puts);
    tcc_add_symbol(s, "printf", save ? (void *)0x44170004 : (void *)printf);
    tcc_add_symbol(s, "putchar", save ? (void *)0x44170008 : (void *)putchar);
    int status = tcc_compile_string(s, source) < 0 || link_program(s, save);
    finish(status);
}
