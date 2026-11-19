// Run in a guest build directory after bootstrapping ./tcc-new.
#include <stdio.h>
#include <lixin.h>

static int run(char **args, int argc) {
    int status = lx_spawn(args[0], argc, args);
    printf("%s: %s (exit %d)\n", args[argc-1], status ? "failed" : "built", status);
    return status != 0;
}

int main(void) {
    char *self[] = {"./tcc-new", "/src/tinycc/bootstrap.c", "-o", "tcc"};
    if (run(self, 4)) return 1;
    static const struct { char *path, *output; } programs[] = {
        {"/src/build/rebuild.c", "rebuild"}, {"/src/bash/bash.c", "bash"},
        {"/src/sh/csh.c", "csh"}, {"/src/ls/ls.c", "ls"},
        {"/src/agent/run.c", "agent-run"}, {"/src/cecho/cecho.c", "cecho"},
        {"/src/net/tcpcheck.c", "tcpcheck"}, {"/src/gui/button.c", "button"},
        {"/src/gui/calculator.c", "calculator"}, {"/src/gui/text-editor.c", "text-editor"},
        {"/src/core/cp.c", "cp"}, {"/src/core/mv.c", "mv"}, {"/src/core/rm.c", "rm"},
        {"/src/core/mkdir.c", "mkdir"}, {"/src/core/rmdir.c", "rmdir"}, {"/src/core/touch.c", "touch"}, {"/src/core/xargs.c", "xargs"},
    };
    int failed = 0;
    for (unsigned i = 0; i < sizeof programs / sizeof *programs; ++i) {
        char *args[] = {"./tcc", programs[i].path, "-o", programs[i].output};
        failed |= run(args, 4);
    }
    char *core[] = {"echo", "cat", "grep", "wc", "pwd", "clear", "netinfo", "cpuinfo", "sysinfo", "uname", "head", "tail", "tee", "true", "false", "sleep", "date", "sort", "uniq", "find", "ps", "df", "stat", "tree", "du", "cmp", "diff", "nl", "cut", "tr", "sed", "seq", "basename", "dirname", "test", "printf"};
    for (unsigned i = 0; i < sizeof core / sizeof *core; ++i) {
        char define[64];
        snprintf(define, sizeof define, "-DAPP=%s", core[i]);
        char *args[] = {"./tcc", define, "/src/core/commands.c", "-o", core[i]};
        failed |= run(args, 5);
    }
    char *desktop[] = {"./tcc", "-I/src/tinycc/sdk", "/src/desktop/desktop.c", "-o", "desktop"};
    failed |= run(desktop, 5);
    char *metrics[] = {"./tcc", "-DAPP_HTOP", "-I/src/tinycc/sdk",
        "/src/foreground/runtime.c", "/src/foreground/htop.c", "-o", "htop"};
    failed |= run(metrics, 7);
    char *paint[] = {"./tcc", "-I/src/tinycc/sdk", "/src/foreground/runtime.c",
        "/src/paint/main.c", "/src/paint/raster.c", "/src/paint/document.c", "-o", "paint"};
    failed |= run(paint, 8);
    char *keys[] = {"/bin/lust", "emit-fn", "keycode", "/src/foreground/keycode.rs", "-o", "keycode.c"};
    if (run(keys, 6)) return 1;
    char *kilo[] = {"./tcc", "-DAPP_KILO", "-I/src/tinycc/sdk", "/src/foreground/runtime.c",
        "/src/kilo/kilo.c", "keycode.c", "-o", "kilo"};
    failed |= run(kilo, 8);
    char *python[] = {"./tcc", "-DNDEBUG", "-I/src/micropython", "-I/src/tinycc/sdk",
        "@/src/micropython/sources.txt", "-o", "python"};
    failed |= run(python, 7);
    char *emit[] = {"/bin/lust", "emit-c", "/src/lust/hello.rs", "-o", "rust-hello.c"};
    if (run(emit, 5)) return 1;
    char *rust[] = {"./tcc", "rust-hello.c", "-o", "rust-hello"};
    return failed | run(rust, 4);
}
