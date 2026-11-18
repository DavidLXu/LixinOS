// Build upstream TinyCC as one translation unit so the freestanding loader
// can reuse its ELF symbol and relocation code without a POSIX dynamic loader.
#define ONE_SOURCE 1
#include "tcc.h"
#undef TCC_IS_NATIVE
#include "libtcc.c"
#include "../console.h"
#include "lixin.h"
#include <stdlib.h>
// Immediate -run code shares the compiler process. Saved programs use the
// resumable implementation at this ABI slot in runner.c.
int lx_spawn(const char *path, int argc, char **argv) {
    (void)path; (void)argc; (void)argv;
    return -1;
}
int lx_spawn_flags(const char *path, int argc, char **argv, unsigned flags) {
    (void)flags; return lx_spawn(path, argc, argv);
}
unsigned lx_start_flags(void) { return 0; }
int lx_agent_yield(void) { return -1; }
void lx_abort(void);
int lx_getchar(void);
FILE *lx_stream(int fd);

#define CODE_BASE 0x44180000UL
#define RUN_CODE_LIMIT (256UL * 1024)

static void diagnostic(void *path, const char *message) {
    printf("%s: %s\n", (char *)path, message);
}

#define DATA_BASE 0x44300000UL
// The runner's RX bank ends at 0x44200000. Its data must stop before
// the utility mailbox. These limits also apply when validating saved images.
#define SAVED_CODE_LIMIT (0x44200000UL - CODE_BASE)
#define SAVED_DATA_LIMIT (0x443d7000UL - DATA_BASE)
#define IMAGE_LIMIT (1024UL * 1024)

// LXE1: magic, entry offset, code length, data length (little-endian u32),
// then relocated code and initialized data, including zero-filled BSS.
static int link_program(TCCState *s, uint64_t *saved, unsigned char *machine_code, int argc, char **argv) {
    const size_t code_limit = saved ? SAVED_CODE_LIMIT : RUN_CODE_LIMIT;
    // Describe initialized ranges to the file service. Blank ranges (including
    // BSS) are zero-filled there, avoiding a second image in the compiler heap.
    struct file_part { uint32_t offset, address, length; };
    struct image_parts { uint32_t count; struct file_part parts[]; } *output = NULL;
    size_t used = 0, data_size = 0;
    resolve_common_syms(s);
    build_got_entries(s, 0);
    for (int i = 1; i < s->nb_sections; ++i) {
        Section *sec = s->sections[i];
        if (!(sec->sh_flags & SHF_ALLOC)) continue;
        if (sec->sh_flags & SHF_EXECINSTR) {
            used = (used + sec->sh_addralign - 1) & -(size_t)sec->sh_addralign;
            if (used > code_limit || sec->data_offset > code_limit - used) {
                puts("tcc: code region full");
                return 1;
            }
            sec->sh_addr = CODE_BASE + used;
            used += sec->data_offset;
        } else if (saved) {
            size_t align = sec->sh_addralign ? sec->sh_addralign : 1;
            data_size = (data_size + align - 1) & -align;
            if (data_size > SAVED_DATA_LIMIT || sec->data_offset > SAVED_DATA_LIMIT - data_size) {
                puts("tcc: executable data region full"); return 1;
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
    if (saved) {
        size_t size = 16 + used + data_size;
        if (size > IMAGE_LIMIT) { puts("tcc: executable exceeds 1 MiB"); return 1; }
        output = tcc_malloc(sizeof(*output) + s->nb_sections * sizeof(struct file_part));
        uint32_t *header = tcc_malloc(16);
        memcpy(header, "LXE1", 4);
        header[1] = entry - CODE_BASE;
        header[2] = used;
        header[3] = data_size;
        output->parts[0] = (struct file_part){0, (uintptr_t)header, 16};
        output->count = 1;
        *saved = ((uint64_t)(uintptr_t)output << 32) | size;
    }
    for (int i = 1; i < s->nb_sections; ++i) {
        Section *sec = s->sections[i];
        if (!(sec->sh_flags & SHF_ALLOC) || !sec->data || !sec->data_offset) continue;
        int code = sec->sh_flags & SHF_EXECINSTR;
        if (saved) {
            size_t offset = code ? 16 + sec->sh_addr - CODE_BASE : 16 + used + sec->sh_addr - DATA_BASE;
            output->parts[output->count++] = (struct file_part){offset, (uintptr_t)sec->data, sec->data_offset};
            continue;
        }
        void *dest = code ? machine_code + (sec->sh_addr - CODE_BASE) : (void *)sec->sh_addr;
        memcpy(dest, sec->data, sec->data_offset);
    }
    if (saved) return 0;
    if (console_call(4, (long)machine_code)) return 1;
    int result = ((int (*)(int, char **))entry)(argc, argv);
    return (unsigned char)result;
}

static TCCState *compiler(const char *path, int save) {
    setbuf(stdout, NULL);
    TCCState *s = tcc_new();
    tcc_set_error_func(s, (void *)path, diagnostic);
    tcc_set_options(s, "-nostdlib -nostdinc");
    tcc_set_output_type(s, TCC_OUTPUT_MEMORY);
    tcc_add_include_path(s, "/include");
    // Slot order is the on-disk LXE1 ABI. Append new symbols after the original three.
    const struct { const char *name; void *address; } symbols[] = {
#define SLOT(name, impl) {#name, impl},
#include "slots.def"
#undef SLOT
    };
    for (unsigned i = 0; i < sizeof symbols / sizeof *symbols; ++i)
        tcc_add_symbol(s, symbols[i].name, save ? (void *)(0x44170000UL + 4*i) : symbols[i].address);
    return s;
}

#ifndef LX_LUST
static unsigned char machine_code[RUN_CODE_LIMIT];
__attribute__((section(".text.start"), noreturn))
void _start(const char *path, const char *source, size_t len, long save) {
    // In -run mode the source terminator is followed by NUL-delimited arguments.
    char *argv[258];
    int argc = 1;
    argv[0] = (char *)path;
    if (!save) {
        size_t offset = strlen(source) + 1;
        while (offset < len) {
            if (argc == 257) finish(1);
            argv[argc++] = (char *)source + offset;
            offset += strlen(source + offset) + 1;
        }
    }
    argv[argc] = NULL;
    TCCState *s = compiler(path, save);
    uint64_t result;
    int status = tcc_compile_string(s, source) < 0 ? 1 : link_program(s, save ? &result : NULL, machine_code, argc, argv);
    if (save && !status) memcpy((void *)0x443d8000UL, &result, sizeof(result));
    finish(status);
}
#else
// Compile all Rust-generated translation units into one TinyCC link state.
extern int lust_compile(const char *path, const char *source, TCCState *state);
int compile_rust_image(const char *path, const char *source, uint64_t *result) {
    TCCState *state = compiler(path, 1);
    return lust_compile(path, source, state) || link_program(state, result, NULL, 0, NULL);
}
#endif
