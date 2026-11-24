#include "../console.h"

/* Spawn Node with the caller's cwd, standard streams and original arguments. */
__attribute__((section(".text.start"), noreturn))
void _start(const char *cwd, const char *args, unsigned long length) {
    (void)cwd;
    static const char prefix[] = LX_SCRIPT_PREFIX;
    char *const mail = (char *)0x443d8000;
    if (length <= 32768 - sizeof prefix) {
        /* The initial argument strings may already occupy the spawn mailbox. */
        char *dest = mail + sizeof prefix;
        if ((unsigned long)dest > (unsigned long)args) {
            for (unsigned long i = length; i; i--) dest[i - 1] = args[i - 1];
        } else {
            for (unsigned long i = 0; i < length; i++) dest[i] = args[i];
        }
        for (unsigned long i = 0; i < sizeof prefix; i++) mail[i] = prefix[i];
        long pid = console_call(50, ((length + sizeof prefix) << 32) | 0x020100);
        if (pid >= 0) {
            long status = console_call(51, pid);
            finish(status >= 0 ? status : 1);
        }
    }
    for (const char *p = LX_SCRIPT_ERROR; *p; p++)
        console_call(11, *p);
    finish(1);
}
