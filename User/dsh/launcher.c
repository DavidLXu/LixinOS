#include "../console.h"

static int file_exists(const char *path) {
    char *mail = (char *)0x443d7000;
    unsigned n = 0;
    while (path[n]) { mail[n] = path[n]; ++n; }
    mail[n] = 0;
    return console_call(7, 0) >= 0;
}

__attribute__((noreturn))
static void launch(const char *prefix, unsigned long prefix_len,
                   const char *args, unsigned long length) {
    char *const mail = (char *)0x443d8000;
    if (length <= 32768 - prefix_len) {
        char *dest = mail + prefix_len;
        if ((unsigned long)dest > (unsigned long)args) {
            for (unsigned long i = length; i; i--) dest[i - 1] = args[i - 1];
        } else {
            for (unsigned long i = 0; i < length; i++) dest[i] = args[i];
        }
        for (unsigned long i = 0; i < prefix_len; i++) mail[i] = prefix[i];
        long pid = console_call(50, ((length + prefix_len) << 32) | 0x020100);
        if (pid >= 0) {
            long status = console_call(51, pid);
            finish(status >= 0 ? status : 1);
        }
    }
    static const char err[] = "dsh: cannot launch Node runtime\n";
    for (const char *p = err; *p; p++)
        console_call(11, *p);
    finish(1);
}

/* Prefer Node 26 (pointer-compressed V8, ~35% faster DSH boot) when paged VM
 * is active and /bin/node26 is installed; otherwise use /bin/node with tuned
 * V8 semi-space sizing to minimize Scavenge GC pauses. */
__attribute__((section(".text.start"), noreturn))
void _start(const char *cwd, const char *args, unsigned long length) {
    (void)cwd;
    static const char p_node26[] =
        ".\0/bin/node26\0/bin/node26\0--max-old-space-size=256\0--max-semi-space-size=16\0"
        "--expose-internals\0--require\0/lib/dsh/fetch.cjs\0/lib/dsh/lib/bin.js";
    static const char p_node22_paged[] =
        ".\0/bin/node\0/bin/node\0--max-old-space-size=256\0--max-semi-space-size=16\0"
        "--expose-internals\0--require\0/lib/dsh/fetch.cjs\0/lib/dsh/lib/bin.js";
    static const char p_node22_small[] =
        ".\0/bin/node\0/bin/node\0--max-old-space-size=128\0--max-semi-space-size=8\0"
        "--expose-internals\0--require\0/lib/dsh/fetch.cjs\0/lib/dsh/lib/bin.js";
    int paged = console_call(62, 0) == 1;
    if (paged && file_exists("/bin/node26"))
        launch(p_node26, sizeof p_node26, args, length);
    if (paged)
        launch(p_node22_paged, sizeof p_node22_paged, args, length);
    launch(p_node22_small, sizeof p_node22_small, args, length);
}
