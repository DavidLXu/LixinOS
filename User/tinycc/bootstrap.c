/* Use the newlib ARM64 varargs ABI and fetch its process-local state
 * through the LXE runtime function table. */
#define LX_SELFHOST 1
#define __VALIST __gnuc_va_list
#define __ASMNAME(name) name
#define _GNU_SOURCE
#include <sys/reent.h>
struct _reent *lx_reent(void);
#undef _REENT
#define _REENT lx_reent()
#include "port.c"
