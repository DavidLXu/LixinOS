// Build upstream TinyCC as one translation unit so the freestanding loader
// can reuse its ELF symbol and relocation code without a POSIX dynamic loader.
#define ONE_SOURCE 1
#include "tcc.h"
#undef TCC_IS_NATIVE
#ifdef LX_SELFHOST
FILE *lx_stream(int);
#undef stdin
#undef stdout
#undef stderr
#define stdin lx_stream(0)
#define stdout lx_stream(1)
#define stderr lx_stream(2)
#endif
#include "libtcc.c"
#ifndef LX_SELFHOST
#include "../console.h"
#include "lixin.h"
#include "../gui/lixin-gui.h"
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
int lx_save_parts(const char *, unsigned long);
void lx_abort(void);
int lx_getchar(void);
FILE *lx_stream(int fd);
#endif

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

struct file_part { uint32_t offset, address, length; };
struct image_parts { uint32_t count; struct file_part parts[]; };

// LXE1 stores its complete data segment, including zero-initialized storage.
enum { LXE_HEADER_SIZE = 16 };
static int link_program(TCCState *s, uint64_t *saved, unsigned char *machine_code, int argc, char **argv) {
    const size_t code_limit = saved ? SAVED_CODE_LIMIT : RUN_CODE_LIMIT;
    // Describe initialized ranges to the file service. Blank ranges (including
    // BSS) are zero-filled there, avoiding a second image in the compiler heap.
    struct image_parts *output = NULL;
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
        size_t size = LXE_HEADER_SIZE + used + data_size;
        if (size > IMAGE_LIMIT) { puts("tcc: executable exceeds 1 MiB"); return 1; }
        output = tcc_malloc(sizeof(*output) + s->nb_sections * sizeof(struct file_part));
        uint32_t *header = tcc_malloc(LXE_HEADER_SIZE);
        memcpy(header, "LXE1", 4);
        header[1] = entry - CODE_BASE;
        header[2] = used;
        header[3] = data_size;
        output->parts[0] = (struct file_part){0, (uintptr_t)header, LXE_HEADER_SIZE};
        output->count = 1;
        *saved = ((uint64_t)(uintptr_t)output << 32) | size;
    }
    for (int i = 1; i < s->nb_sections; ++i) {
        Section *sec = s->sections[i];
        if (!(sec->sh_flags & SHF_ALLOC) || sec->sh_type == SHT_NOBITS || !sec->data || !sec->data_offset) continue;
        int code = sec->sh_flags & SHF_EXECINSTR;
        if (saved) {
            size_t offset = LXE_HEADER_SIZE + (code ? sec->sh_addr - CODE_BASE : used + sec->sh_addr - DATA_BASE);
            output->parts[output->count++] = (struct file_part){offset, (uintptr_t)sec->data, sec->data_offset};
            continue;
        }
        void *dest = code ? machine_code + (sec->sh_addr - CODE_BASE) : (void *)sec->sh_addr;
        memcpy(dest, sec->data, sec->data_offset);
    }
    if (saved) return 0;
#ifndef LX_SELFHOST
    if (console_call(4, (long)machine_code)) return 1;
    int result = ((int (*)(int, char **))entry)(argc, argv);
    return (unsigned char)result;
#else
    return 1;
#endif
}

#ifndef LX_SELFHOST
#define SLOT(name, impl)
#define LIBC(name) extern void rt_##name(void) asm(#name);
#include "slots.def"
#undef SLOT
#undef LIBC
#endif

static TCCState *compiler(const char *path, int save, int kernel) {
    setbuf(stdout, NULL);
    TCCState *s = tcc_new();
    tcc_set_error_func(s, (void *)path, diagnostic);
    tcc_set_options(s, "-nostdlib -nostdinc");
    tcc_set_output_type(s, TCC_OUTPUT_MEMORY);
    char directory[1024], inc[1024];
    pstrcpy(directory, sizeof directory, path);
    char *slash = strrchr(directory, '/');
    if (slash) *slash = 0; else strcpy(directory, ".");
    const char *subdirs[] = {"", "/sdk", "/upstream", "/builtin"};
    for (int i = 0; i < sizeof subdirs / sizeof *subdirs; i++) {
        snprintf(inc, sizeof inc, "%s%s", directory, subdirs[i]);
        tcc_add_include_path(s, inc);
    }
    tcc_add_include_path(s, "/include");
    tcc_add_include_path(s, "/src/tinycc/builtin");
    if (kernel) return s;
    // Keep runtime slots stable across LXE versions. Append new symbols.
    const struct { const char *name; void *address; } symbols[] = {
#ifdef LX_SELFHOST
#define SLOT(name, impl) {#name, NULL},
#define LIBC(name) {#name, NULL},
#else
#define SLOT(name, impl) {#name, impl},
#define LIBC(name) {#name, rt_##name},
#endif
#include "slots.def"
#undef SLOT
#undef LIBC
    };
    for (unsigned i = 0; i < sizeof symbols / sizeof *symbols; ++i)
        tcc_add_symbol(s, symbols[i].name, save ? (void *)(0x44170000UL + 4*i) : symbols[i].address);
    return s;
}

#ifdef LX_SELFHOST
#include "kernel-elf.h"
static int add_inputs(TCCState *s, const char *name, const char *output) {
    if (*name != '@') return tcc_add_file(s, name);
    FILE *list = fopen(name + 1, "r");
    if (!list) { printf("cannot read %s\n", name+1); return -1; }
    char line[515];
    int result = 0;
    while (fgets(line, sizeof line, list)) {
        size_t n = strlen(line);
        if (n && line[n-1] == '\n') line[--n] = 0;
        if (n && line[n-1] == '\r') line[--n] = 0;
        if (n <= 512 && n > 2 && !strncmp(line, "-I", 2)) {
            if (tcc_add_include_path(s, line + 2) < 0) { result = -1; break; }
            continue;
        }
        if (n && (n > 512 || !strcmp(output, line) || tcc_add_file(s,line) < 0)) {
            result = -1; break;
        }
    }
    if (ferror(list)) result = -1;
    fclose(list);
    return result;
}
/* A saved LXE runs with its own stack, data, argument vector and process ID.
 * Exclusive creation prevents a prior invocation's file being overwritten. */
static int run_source(int argc, char **argv) {
    extern long lx_console_call(long, long);
    extern int lx_spawn(const char *, int, char **), lx_pid(void);
    TCCState *state = compiler(argv[0], 1, 0);
    uint64_t image;
    if (tcc_add_file(state, argv[0]) < 0 || link_program(state, &image, NULL, 0, NULL)) return 1;
    char output[80];
    snprintf(output, sizeof(output), "/.tcc-run-%d-%lu", lx_pid(),
             (unsigned long)lx_console_call(38, 2));
    char *mail = (char *)0x443d7000UL;
    strcpy(mail, output);
    if (lx_console_call(31, image) < 0) { puts("tcc: could not create executable"); return 1; }
    int status = lx_spawn(output, argc, argv);
    strcpy(mail, output);
    if (lx_console_call(42, 3) < 0) { puts("tcc: could not remove temporary executable"); return 1; }
    return status < 0 ? 1 : status;
}
int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "-run")) return run_source(argc - 2, argv + 2);
    if (argc < 4 || strcmp(argv[argc-2], "-o")) {
        puts("usage: tcc [--kernel|--bootloader] source.c [more.c ...] -o output"); return 1;
    }
    int first = 1;
    while (first < argc-2 && argv[first][0] == '-') ++first;
    if (first == argc-2) return 1;
    int kernel = 0;
    for (int i = 1; i < first; ++i) {
        int mode = !strcmp(argv[i], "--kernel") ? 1 : !strcmp(argv[i], "--bootloader") ? 2 : 0;
        if (mode && kernel && kernel != mode) { puts("tcc: conflicting load regions"); return 1; }
        if (mode) kernel = mode;
    }
    TCCState *s = compiler(argv[first], 1, kernel);
    for (int i = 1; i < first; ++i) {
        char *opt = argv[i];
        if (!strcmp(opt, "--kernel") || !strcmp(opt, "--bootloader")) continue;
        if (!strncmp(opt, "-I", 2) && opt[2]) tcc_add_include_path(s, opt+2);
        else if (!strncmp(opt, "-D", 2) && opt[2]) {
            char *value = strchr(opt+2, '=');
            if (value) *value++ = 0;
            tcc_define_symbol(s, opt+2, value ? value : "1");
        } else { printf("unsupported option %s\n", opt); return 1; }
    }
    uint64_t descriptor;
    for (int i = first; i < argc-2; ++i) {
        if (!strcmp(argv[i], argv[argc-1])) { puts("output is an input file"); return 1; }
        if (add_inputs(s, argv[i], argv[argc-1]) < 0) return 1;
    }
    if (kernel) {
        if (link_kernel(s, &descriptor, kernel == 2)) return 1;
    } else if (link_program(s, &descriptor, NULL, 0, NULL)) return 1;
    extern int lx_save_parts(const char *, uint64_t);
    return lx_save_parts(argv[argc-1], descriptor) < 0;
}
#elif !defined(LX_LUST)
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
    TCCState *s = compiler(path, save, 0);
    uint64_t result;
    int status = tcc_compile_string(s, source) < 0 ? 1 : link_program(s, save ? &result : NULL, machine_code, argc, argv);
    if (save && !status) memcpy((void *)0x443d8000UL, &result, sizeof(result));
    finish(status);
}
#else
// Compile all Rust-generated translation units into one TinyCC link state.
extern int lust_compile(const char *path, const char *source, TCCState *state);
int compile_rust_image(const char *path, const char *source, uint64_t *result) {
    TCCState *state = compiler(path, 1, 0);
    return lust_compile(path, source, state) || link_program(state, result, NULL, 0, NULL);
}
#endif
