#pragma once
#include <glib.h>

/* The callbacks run on GTK's main loop. Lines from stderr are diagnostics. */
typedef void (*DshLine)(const char *line, gboolean diagnostic);
typedef void (*DshDone)(int status);
int dsh_start(const char *directory, const char *session, const char *prompt, DshLine line, DshDone done);
gboolean dsh_busy(void);
