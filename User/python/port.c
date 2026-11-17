#include "../console.h"
#include "py/runtime.h"
#include "py/lexer.h"
#include "py/mperrno.h"
#include "py/cstack.h"
#include "py/gc.h"
#include "shared/readline/readline.h"
#include "shared/runtime/gchelper.h"
#include "shared/runtime/pyexec.h"

static const char *script_source;
static size_t script_length;

static uintptr_t heap[512 * 1024 / sizeof(uintptr_t)];

void lixin_poll_status(void) { console_call(3, 0); }

int mp_hal_stdin_rx_chr(void) {
    int c = get_byte();
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

__attribute__((section(".text.start"), noreturn)) void _start(const char *path, const char *source, size_t length) {
    mp_cstack_init_with_sp_here(128 * 1024);
    gc_init(heap, heap + sizeof(heap) / sizeof(*heap));
    mp_init();
    int status = 0;
    if (path) {
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
