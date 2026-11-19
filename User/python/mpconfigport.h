#ifdef __TINYC__
#define MICROPY_NLR_SETJMP 1
#define MICROPY_GCREGS_SETJMP 1
#define MICROPY_NO_ALLOCA 1
static inline int lx_mp_clzl(unsigned long x) {
    int n = 0;
    while (n < 64 && !(x >> 63)) { ++n; x <<= 1; }
    return n;
}
#define __builtin_clzl lx_mp_clzl
#endif
#include <stdint.h>
#include <alloca.h>
#define MICROPY_CONFIG_ROM_LEVEL MICROPY_CONFIG_ROM_LEVEL_MINIMUM
#define MICROPY_ENABLE_COMPILER 1
#define MICROPY_ENABLE_GC 1
#define MICROPY_STACK_CHECK 1
#define MICROPY_HELPER_REPL 1
#define MICROPY_ERROR_REPORTING MICROPY_ERROR_REPORTING_NORMAL
#define MICROPY_HW_BOARD_NAME "LixinOS"
#define MICROPY_HW_MCU_NAME "AArch64 EL0"
typedef intptr_t mp_int_t;
typedef uintptr_t mp_uint_t;
typedef long mp_off_t;
#define MP_STATE_PORT MP_STATE_VM
#define MICROPY_FLOAT_IMPL MICROPY_FLOAT_IMPL_DOUBLE
#define MICROPY_LONGINT_IMPL MICROPY_LONGINT_IMPL_MPZ
#define MICROPY_PY_BUILTINS_INPUT 1
#define MICROPY_ENABLE_SOURCE_LINE 1

// Give the guest status bar a sampling point during compute-only loops.
void lixin_poll_status(void);
#define MICROPY_VM_HOOK_LOOP do { static unsigned iterations; if (++iterations == 8192) { iterations = 0; lixin_poll_status(); } } while (0);

#define MICROPY_PY_SYS 1
#define MICROPY_PY_SYS_PLATFORM "lixinos"
#define MICROPY_PY_SYS_STDFILES 1
#define MICROPY_PY_IO 1
#define MICROPY_PY_SELECT 1
#define MICROPY_ENABLE_FINALISER 1
#define MICROPY_PY_RANDOM 1
#define MICROPY_PY_RANDOM_EXTRA_FUNCS 1
#define MICROPY_MODULE_BUILTIN_INIT 1
unsigned lixin_random_seed(void);
#define MICROPY_PY_RANDOM_SEED_INIT_FUNC lixin_random_seed()
#define MICROPY_PY_TIME 1
#define MICROPY_PY_TIME_TIME_TIME_NS 1
#define MICROPY_PY_TIME_INCLUDEFILE "lixin-time.h"
#define MICROPY_PY_TIME_CUSTOM_SLEEP 1

#define MICROPY_PY_ATTRTUPLE 1
#define MICROPY_PY_BUILTINS_STR_OP_MODULO 1
#define MICROPY_PY_BUILTINS_SLICE 1
#define MICROPY_PY_BUILTINS_SLICE_ATTRS 1
#define MICROPY_PY_BUILTINS_SLICE_INDICES 1
#define MICROPY_PY_BUILTINS_SET 1
#define MICROPY_PY_BUILTINS_MIN_MAX 1
#define MICROPY_PY_BUILTINS_ENUMERATE 1
#define MICROPY_PY_BUILTINS_REVERSED 1
#define MICROPY_PY_FSTRINGS 1
#define MICROPY_PY_ASSIGN_EXPR 1

void lixin_wait_for_event(void);
#define MICROPY_INTERNAL_WFE(timeout_ms) ((void)(timeout_ms), lixin_wait_for_event())
