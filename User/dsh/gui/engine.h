#pragma once
#include <glib.h>

/* The callbacks run on GTK's main loop. Lines from stderr are diagnostics. */
typedef void (*DshLine)(const char *line, gboolean diagnostic);
typedef void (*DshDone)(int status);
void dsh_warmup(const char *directory);
int dsh_start(const char *directory, const char *session, const char *prompt, DshLine line, DshDone done);
int dsh_cancel(void);
void dsh_reset(void);
void dsh_shutdown(void);
gboolean dsh_busy(void);
gboolean dsh_ready(void);
