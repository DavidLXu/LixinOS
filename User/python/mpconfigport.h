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
