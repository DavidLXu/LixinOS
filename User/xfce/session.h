#pragma once
#include <string.h>
#define XFCE_RECORD "\036"
enum { PANEL_HEIGHT = 44, MAX_CLIENTS = 4 };

static int window_name(const char *name) {
    const char *suffix = strrchr(name, '.');
    return !strchr(name, '/') && !strpbrk(name, "\r\n" XFCE_RECORD) &&
        (!strcmp(name, "nxeyes") || (suffix && (!strcmp(suffix, ".win") ||
            !strcmp(suffix, ".win2") || !strcmp(suffix, ".win3"))));
}
