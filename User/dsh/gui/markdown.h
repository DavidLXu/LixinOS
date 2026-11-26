#pragma once

#include <gtk/gtk.h>

void markdown_setup(GtkTextView *view);
void markdown_enable_links(GtkTextView *view);
void markdown_linkify(GtkTextBuffer *buffer, int start, const char *source, GPtrArray *tags, gboolean dark);
/* Remove render-specific tags when discarding their text. */
void markdown_append(GtkTextBuffer *buffer, const char *source, GPtrArray *links);
void markdown_clear_links(GtkTextBuffer *buffer, GPtrArray *links);
