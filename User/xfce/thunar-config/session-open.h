#pragma once
#include <gio/gio.h>
#include <unistd.h>
#include "../session.h"

/* A graphical launch keeps stdout for pixels and passes the session a local
 * filename on a private pipe. Hex preserves whitespace and line breaks. */
static gboolean lixin_open_text(ThunarFile *file) {
    if (!g_getenv("LIXINOS_XFCE_SESSION")) return FALSE;
    const char *type = thunar_file_get_content_type(file);
    const char *name = thunar_file_get_display_name(file);
    if (!(type && g_content_type_is_a(type, "text/plain")) && !g_str_has_suffix(name, ".txt"))
        return FALSE;
    g_autofree char *path = g_file_get_path(thunar_file_get_file(file));
    if (!path || !g_path_is_absolute(path) || strlen(path) > 512) return FALSE;
    char message[1032] = XFCE_RECORD "EDIT ";
    const char hex[] = "0123456789abcdef";
    size_t n = sizeof(XFCE_RECORD "EDIT ") - 1;
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p) {
        message[n++] = hex[*p >> 4];
        message[n++] = hex[*p & 15];
    }
    message[n++] = '\n';
    return write(STDERR_FILENO, message, n) == (ssize_t)n;
}
