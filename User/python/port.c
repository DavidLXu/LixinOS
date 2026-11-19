#include "../console.h"
#include "py/runtime.h"
#include "py/lexer.h"
#include "py/mperrno.h"
#include "py/cstack.h"
#include "py/gc.h"
#include "py/builtin.h"
#include "py/objlist.h"
#include <string.h>
#include "shared/readline/readline.h"
#include "shared/runtime/gchelper.h"
#include "shared/runtime/pyexec.h"

static const char *script_source;
static size_t script_length;

#ifdef __TINYC__
#include <lixin.h>
#include "py/builtin.h"
mp_import_stat_t mp_import_stat(const char *path) {
    (void)path; return MP_IMPORT_STAT_NO_EXIST;
}
#include <stdlib.h>
static uintptr_t *heap;
#else
static uintptr_t heap[512 * 1024 / sizeof(uintptr_t)];
#endif

void lixin_poll_status(void) { console_call(3, 0); }

int mp_hal_stdin_rx_chr(void) {
    int c = console_call(0, 1);
    if (c < 0 || c > 255) return 4;
    return c == '\n' ? '\r' : c;
}

int lixin_readline(vstr_t *line, const char *prompt) {
    if (!console_call(5, 0)) {
        return readline(line, prompt);
    }
    // Agent tests supply stdin as a byte stream without terminal echo.
    for (;;) {
        int c = get_byte();
        if (c == 256) {
            return CHAR_CTRL_D;
        }
        if (c == '\n') {
            return 0;
        }
        vstr_add_byte(line, c);
    }
}

mp_uint_t mp_hal_stdout_tx_strn(const char *s, size_t n) {
    for (size_t i = 0; i < n; ++i) put_byte(s[i]);
    return n;
}

void gc_collect(void) {
    gc_collect_start();
    gc_helper_collect_regs_and_stack();
    gc_collect_end();
}

void nlr_jump_fail(void *exception) {
    (void)exception;
    finish(1);
}

#ifdef __TINYC__
int main(int argc, char **argv) {
    int inline_script = argc > 1 && strcmp(argv[1], "-c") == 0;
    if (inline_script && argc < 3) return 1;
    char *path = inline_script ? "<string>" : argc > 1 ? argv[1] : NULL;
    size_t length = 0;
    char *source = NULL;
    if (inline_script) {
        source = argv[2];
        length = strlen(source);
    } else if (path) {
        long size = lx_stat(path);
        if (size < 0) {
            lx_error(size == -2 ? "python: is a directory\n" : "python: file not found\n");
            return 1;
        }
        if (!(source = malloc(size+1))) return 1;
        int fd = lx_open(path);
        if (fd < 0) return 1;
        while (length < (size_t)size) {
            int n = lx_read(fd, source + length, size-length);
            if (n <= 0) return 1;
            length += n;
        }
        lx_close(fd);
        source[length] = 0;
    }
    heap = malloc(512*1024);
    if (!heap) return 1;
#else
__attribute__((section(".text.start"), noreturn)) void _start(const char *path, const char *source, size_t length) {
#endif
    mp_cstack_init_with_sp_here(128 * 1024);
    gc_init(heap, heap + 512 * 1024 / sizeof(*heap));
    mp_init();
    int status = 0;
    if (path) {
        mp_obj_list_append(MP_OBJ_FROM_PTR(&MP_STATE_VM(mp_sys_argv_obj)), mp_obj_new_str(path, strlen(path)));
#ifdef __TINYC__
        for (int i = inline_script ? 3 : 2; i < argc; ++i) {
            mp_obj_list_append(MP_OBJ_FROM_PTR(&MP_STATE_VM(mp_sys_argv_obj)), mp_obj_new_str(argv[i], strlen(argv[i])));
        }
#endif
        script_source = source;
        script_length = length;
        status = pyexec_file(path) == 1 ? 0 : 1;
    } else {
        pyexec_friendly_repl();
    }
    mp_deinit();
    finish(status);
}

mp_lexer_t *mp_lexer_new_from_file(qstr path) {
    if (script_source) {
        const char *source = script_source;
        script_source = NULL;
        return mp_lexer_new_from_str_len(path, source, script_length, 0);
    }
    mp_raise_OSError(MP_ENOENT);
}
