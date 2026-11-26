#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "engine.h"
#include "markdown.h"

/* One immutable filename per turn, so separate windows cannot overwrite one
 * another's transcript. The engine remains the owner of session continuity. */
typedef struct {
    char *file;
    JsonObject *data;
} Turn;
static GPtrArray *turns;
static GHashTable *pending_searches;
static Turn *active;
static char *archive, *thread_id, *session_id, *thread_title;
static gboolean final_seen, stop_accepted;
static char *stop_file, *last_text;
static GtkTextMark *preview_mark;
static GString *preview_source;
static GPtrArray *chat_links, *preview_links, *trace_links;
static guint preview_timer;
static GtkTextMark *web_mark;
static guint searches, fetches, with_sources, empty_searches, failed_searches;
static GtkWidget *window, *sidebar, *history, *folder, *title, *status, *pages;
static GtkWidget *editor, *send_button, *new_button, *welcome, *tab, *placeholder;
static GtkWidget *chat_page, *search;
static GtkTextBuffer *chat, *trace;
static GString *diagnostic;
enum TraceKind { USER, CALL, RESULT, ASSISTANT, INFO };
typedef struct {
    enum TraceKind kind;
    GtkTextMark *mark;
} TracePoint;
static GArray *trace_points;
static GtkWidget *timeline, *trace_count;
static guint trace_turns, trace_calls;

static const char *field(JsonObject *o, const char *key) {
    return json_object_get_string_member_with_default(o, key, "");
}
static void replace(char **old, const char *value) {
    char *next = g_strdup(value);
    g_free(*old);
    *old = next;
}
static void note(const char *s) { gtk_label_set_text(GTK_LABEL(status), s); }
static GtkWidget *label(const char *text, const char *name) {
    GtkWidget *w = gtk_label_new(text);
    gtk_widget_set_name(w, name);
    gtk_label_set_xalign(GTK_LABEL(w), 0);
    gtk_label_set_ellipsize(GTK_LABEL(w), PANGO_ELLIPSIZE_END);
    return w;
}
static void pack(GtkWidget *box, GtkWidget *item, gboolean fill) {
    gtk_box_pack_start(GTK_BOX(box), item, fill, fill, 0);
}
static GtkWidget *button(const char *text, const char *name, GCallback fn) {
    GtkWidget *w = gtk_button_new_with_label(text);
    gtk_widget_set_name(w, name);
    if (fn)
        g_signal_connect(w, "clicked", fn, NULL);
    return w;
}
static char *encode(JsonObject *o) {
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, o);
    char *s = json_to_string(node, FALSE);
    json_node_free(node);
    return s;
}
static gboolean save_turn(Turn *turn) {
    g_autofree char *data = encode(turn->data);
    g_autoptr(GError) error = NULL;
    if (!g_file_set_contents(turn->file, data, -1, &error)) {
        note(error->message);
        return FALSE;
    }
    return TRUE;
}
static void release_turn(gpointer value) {
    Turn *turn = value;
    g_free(turn->file);
    json_object_unref(turn->data);
    g_free(turn);
}
static gint by_time(gconstpointer a, gconstpointer b) {
    return strcmp((*(Turn *const *)a)->file, (*(Turn *const *)b)->file);
}
static void load_turns(void) {
    GDir *dir = g_dir_open(archive, 0, NULL);
    if (!dir)
        return;
    const char *name;
    while ((name = g_dir_read_name(dir))) {
        if (!g_str_has_suffix(name, ".json"))
            continue;
        g_autofree char *path = g_build_filename(archive, name, NULL);
        g_autoptr(JsonParser) parser = json_parser_new();
        if (!json_parser_load_from_file(parser, path, NULL))
            continue;
        JsonNode *node = json_parser_get_root(parser);
        if (!node || !JSON_NODE_HOLDS_OBJECT(node))
            continue;
        JsonObject *o = json_node_get_object(node);
        JsonNode *events = json_object_get_member(o, "events");
        if (!*field(o, "thread") || !events || !JSON_NODE_HOLDS_ARRAY(events))
            continue;
        Turn *turn = g_new0(Turn, 1);
        turn->file = g_strdup(path);
        turn->data = json_object_ref(o);
        g_ptr_array_add(turns, turn);
    }
    g_dir_close(dir);
    g_ptr_array_sort(turns, by_time);
}
static GtkWidget *transcript(GtkTextBuffer **buffer) {
    GtkWidget *view = gtk_text_view_new();
    gboolean console = buffer == &trace;
    gtk_widget_set_name(view, console ? "trace-console" : "chat-transcript");
    if (!console)
        gtk_text_view_set_pixels_inside_wrap(GTK_TEXT_VIEW(view), 2);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 17);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), 17);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), 12);
    *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gtk_text_buffer_create_tag(*buffer, "name", "foreground", console ? "#a1c3ff" : "#426aca", "weight",
                               PANGO_WEIGHT_BOLD, "pixels-above-lines", 9, NULL);
    gtk_text_buffer_create_tag(*buffer, "tool", "foreground", console ? "#c6cfdf" : "#626b79", "family",
                               "monospace", "scale", console ? 1.0 : 0.88, NULL);
    markdown_enable_links(GTK_TEXT_VIEW(view));
    if (!console)
        markdown_setup(GTK_TEXT_VIEW(view));
    gtk_text_buffer_create_tag(*buffer, "error", "foreground", console ? "#ffa8a4" : "#ab3a36", NULL);
    GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroller), view);
    g_object_set_data(G_OBJECT(*buffer), "view", view);
    return scroller;
}
static void insert(GtkTextBuffer *buffer, const char *s, const char *style) {
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    if (style)
        gtk_text_buffer_insert_with_tags_by_name(buffer, &end, s, -1, style, NULL);
    else
        gtk_text_buffer_insert(buffer, &end, s, -1);
}
static void finish_web_group(void) {
    if (web_mark)
        gtk_text_buffer_delete_mark(chat, web_mark);
    web_mark = NULL;
    searches = fetches = with_sources = empty_searches = failed_searches = 0;
}
static void web_activity(void) {
    GtkTextIter start, end;
    if (web_mark) {
        gtk_text_buffer_get_iter_at_mark(chat, &start, web_mark);
        end = start;
        gtk_text_iter_forward_line(&end);
        gtk_text_buffer_delete(chat, &start, &end);
    } else {
        insert(chat, "DeepSeek\n", "name");
        gtk_text_buffer_get_end_iter(chat, &start);
        web_mark = gtk_text_buffer_create_mark(chat, NULL, &start, TRUE);
    }
    GString *line = g_string_new("Web");
    if (searches) g_string_append_printf(line, "  ·  %u search%s", searches, searches == 1 ? "" : "es");
    if (fetches) g_string_append_printf(line, "  ·  %u page request%s", fetches, fetches == 1 ? "" : "s");
    if (with_sources) g_string_append(line, "  ·  Sources found");
    if (empty_searches) g_string_append_printf(line, "  ·  %u without sources", empty_searches);
    if (failed_searches) g_string_append_printf(line, "  ·  %u failed", failed_searches);
    g_string_append_c(line, '\n');
    gtk_text_buffer_insert_with_tags_by_name(chat, &start, line->str, line->len,
                                             failed_searches ? "error" : "tool", NULL);
    g_string_free(line, TRUE);
}
static void message(GtkTextBuffer *b, const char *who, const char *s, const char *style) {
    gboolean reply = b == chat && !strcmp(who, "DeepSeek");
    if (!reply || !web_mark) {
        insert(b, who, "name");
        insert(b, "\n", NULL);
    }
    if (b == chat) finish_web_group();
    if (reply)
        markdown_append(b, s, chat_links);
    else {
        int start = gtk_text_buffer_get_char_count(b);
        insert(b, s, style);
        if (b == chat || !strcmp(who, "User") || g_str_has_suffix(who, " · completed"))
            markdown_linkify(b, start, s, b == chat ? chat_links : trace_links, b == trace);
        insert(b, "\n", NULL);
    }
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(b, &end);
    GtkTextMark *mark = gtk_text_buffer_get_mark(b, "tail");
    if (mark)
        gtk_text_buffer_move_mark(b, mark, &end);
    else
        mark = gtk_text_buffer_create_mark(b, "tail", &end, FALSE);
    gtk_text_view_scroll_mark_onscreen(g_object_get_data(G_OBJECT(b), "view"), mark);
}
static void update_timeline(void) {
    g_autofree char *caption =
        g_strdup_printf("%u turn%s    %u tool call%s", trace_turns, trace_turns == 1 ? "" : "s", trace_calls,
                        trace_calls == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(trace_count), caption);
    gtk_widget_queue_draw(timeline);
}
static void trace_message(enum TraceKind kind, const char *who, const char *content, const char *style) {
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(trace, &end);
    TracePoint point = {kind, gtk_text_buffer_create_mark(trace, NULL, &end, TRUE)};
    g_array_append_val(trace_points, point);
    trace_turns += kind == USER;
    trace_calls += kind == CALL;
    message(trace, who, content, style);
    update_timeline();
}
// Each visible segment addresses the corresponding entry in the underlying text buffer.
static double segment_width(GtkWidget *w) {
    return MIN(22.0, gtk_widget_get_allocated_width(w) / (double)MAX(1u, trace_points->len));
}
static gboolean draw_timeline(GtkWidget *w, cairo_t *cr, gpointer unused) {
    (void)unused;
    static const double colors[][3] = {
        {0.47, 0.70, 1.00}, {0.96, 0.66, 0.30}, {0.43, 0.82, 0.63}, {0.73, 0.59, 0.98}, {0.78, 0.80, 0.84}};
    double width = segment_width(w);
    for (guint i = 0; i < trace_points->len; ++i) {
        const double *color = colors[g_array_index(trace_points, TracePoint, i).kind];
        cairo_set_source_rgb(cr, color[0], color[1], color[2]);
        cairo_rectangle(cr, i * width, 5, MAX(width * 0.6, width - 2), 6);
        cairo_fill(cr);
    }
    return FALSE;
}
static gboolean jump_to_event(GtkWidget *w, GdkEventButton *event, gpointer unused) {
    (void)unused;
    if (event->button != 1 || event->x < 0 || !trace_points->len)
        return FALSE;
    guint index = event->x / segment_width(w);
    if (index >= trace_points->len)
        return FALSE;
    GtkTextView *view = g_object_get_data(G_OBJECT(trace), "view");
    gtk_text_view_scroll_to_mark(view, g_array_index(trace_points, TracePoint, index).mark, 0, TRUE, 0, 0);
    return TRUE;
}
static void clear_timeline(void) {
    for (guint i = 0; i < trace_points->len; ++i)
        gtk_text_buffer_delete_mark(trace, g_array_index(trace_points, TracePoint, i).mark);
    g_array_set_size(trace_points, 0);
    trace_turns = trace_calls = 0;
    update_timeline();
}
static void erase_preview(void) {
    GtkTextIter from, end;
    gtk_text_buffer_get_iter_at_mark(chat, &from, preview_mark);
    gtk_text_buffer_get_end_iter(chat, &end);
    gtk_text_buffer_delete(chat, &from, &end);
    markdown_clear_links(chat, preview_links);
}
static gboolean draw_preview(gpointer unused) {
    (void)unused;
    preview_timer = 0;
    erase_preview();
    if (!web_mark)
        insert(chat, "DeepSeek\n", "name");
    markdown_append(chat, preview_source->str, preview_links);
    return G_SOURCE_REMOVE;
}
static void clear_preview(void) {
    if (!preview_mark)
        return;
    g_clear_handle_id(&preview_timer, g_source_remove);
    erase_preview();
    gtk_text_buffer_delete_mark(chat, preview_mark);
    preview_mark = NULL;
    g_string_truncate(preview_source, 0);
}
static void render_event(JsonObject *o, gboolean live) {
    const char *kind = field(o, "type");
    const char *text = field(o, "text");
    if (!strcmp(kind, "preview_text")) {
        if (!preview_mark) {
            GtkTextIter end;
            gtk_text_buffer_get_end_iter(chat, &end);
            preview_mark = gtk_text_buffer_create_mark(chat, NULL, &end, TRUE);
        }
        g_string_append(preview_source, text);
        if (!preview_timer)
            preview_timer = g_timeout_add(120, draw_preview, NULL);
        return;
    }
    clear_preview();
    if (!strcmp(kind, "stop_accepted")) {
        trace_message(INFO, "Stop", "Requested by user", NULL);
        if (live) {
            stop_accepted = TRUE;
            note("Stopping…");
        }
    } else if (!strcmp(kind, "session")) {
        if (live) {
            replace(&session_id, field(o, "sessionId"));
            json_object_set_string_member(active->data, "session", session_id);
            save_turn(active);
        }
    } else if (!strcmp(kind, "status")) {
        if (!strcmp(field(o, "phase"), "step_end") && json_object_has_member(o, "usage")) {
            JsonObject *usage = json_object_get_object_member(o, "usage");
            if (usage) {
                g_autofree char *stats =
                    g_strdup_printf("Step %" G_GINT64_FORMAT " · %" G_GINT64_FORMAT
                                    " input / %" G_GINT64_FORMAT " output tokens\n",
                                    json_object_get_int_member_with_default(o, "step", 0),
                                    json_object_get_int_member_with_default(usage, "inputTokens", 0),
                                    json_object_get_int_member_with_default(usage, "outputTokens", 0));
                insert(trace, stats, "tool");
            }
        }
        if (live && !strcmp(field(o, "phase"), "step_start")) {
            g_autofree char *s = g_strdup_printf("Running · Step %" G_GINT64_FORMAT,
                                                 json_object_get_int_member_with_default(o, "step", 0));
            note(s);
        }
    } else if (!strcmp(kind, "tool_call")) {
        const char *name = field(o, "tool");
        if (!strcmp(name, "web_search"))
            g_hash_table_add(pending_searches, g_strdup(field(o, "callId")));
        JsonNode *input = json_object_get_member(o, "input");
        g_autofree char *args = input ? json_to_string(input, FALSE) : g_strdup("");
        g_autofree char *summary = g_strdup_printf("Tool · %s", name);
        if (!strcmp(name, "web_search") || !strcmp(name, "web_fetch")) {
            if (!strcmp(name, "web_search")) searches++;
            else fetches++;
            web_activity();
        } else {
            insert(chat, summary, "tool");
            insert(chat, "\n", NULL);
        }
        trace_message(CALL, summary, args, "tool");
        if (live)
            note(summary);
    } else if (!strcmp(kind, "tool_result")) {
        g_autofree char *heading = g_strdup_printf("%s · %s", field(o, "callId"), field(o, "status"));
        if (g_hash_table_remove(pending_searches, field(o, "callId"))) {
            const char *state = field(o, "status");
            if (strcmp(state, "completed")) failed_searches++;
            else if (strstr(field(o, "result"), "\nNo results found.")) empty_searches++;
            else with_sources++;
            web_activity();
        }
        trace_message(RESULT, heading, field(o, "result"),
                      !strcmp(field(o, "status"), "error") ? "error" : "tool");
    } else if (!strcmp(kind, "thinking") || !strcmp(kind, "text")) {
        trace_message(ASSISTANT, !strcmp(kind, "text") ? "Assistant" : "Thinking", text, NULL);
        if (!strcmp(kind, "text") && *text) {
            replace(&last_text, text);
            // The headless projection may truncate a committed text block. Its
            // lossless final event replaces that block when it arrives.
            if (!json_object_get_boolean_member_with_default(o, "truncated", FALSE))
                message(chat, "DeepSeek", text, NULL);
            else
                replace(&last_text, "");
        }
    } else if (!strcmp(kind, "final")) {
        if (*text && g_strcmp0(text, last_text))
            message(chat, "DeepSeek", text, NULL);
        if (live)
            final_seen = TRUE;
    } else if (!strcmp(kind, "error")) {
        message(chat, "Engine", field(o, "message"), "error");
        trace_message(INFO, "Engine", field(o, "message"), "error");
    }
}
static void select_thread(const char *id) {
    if (dsh_busy())
        return;
    replace(&thread_id, id);
    replace(&session_id, "");
    replace(&thread_title, "");
    finish_web_group();
    markdown_clear_links(chat, chat_links);
    gtk_text_buffer_set_text(chat, "", 0);
    clear_timeline();
    markdown_clear_links(trace, trace_links);
    gtk_text_buffer_set_text(trace, "", 0);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(editor)), "", 0);
    gboolean found = FALSE;
    for (guint i = 0; i < turns->len; i++) {
        JsonObject *o = ((Turn *)g_ptr_array_index(turns, i))->data;
        if (strcmp(thread_id, field(o, "thread")))
            continue;
        found = TRUE;
        if (*field(o, "session"))
            replace(&session_id, field(o, "session"));
        replace(&thread_title, field(o, "title"));
        gtk_entry_set_text(GTK_ENTRY(folder), field(o, "cwd"));
        replace(&last_text, "");
        message(chat, "You", field(o, "prompt"), NULL);
        trace_message(USER, "User", field(o, "prompt"), NULL);
        g_hash_table_remove_all(pending_searches);
        JsonArray *events = json_object_get_array_member(o, "events");
        for (guint n = 0; n < json_array_get_length(events); n++) {
            JsonNode *e = json_array_get_element(events, n);
            if (JSON_NODE_HOLDS_OBJECT(e))
                render_event(json_node_get_object(e), FALSE);
        }
        if (!json_object_get_boolean_member_with_default(o, "closed", FALSE))
            message(chat, "Engine", "The previous turn ended before its result was saved.", "error");
    }
    gtk_label_set_text(GTK_LABEL(title), found ? thread_title : "New session");
    gtk_stack_set_visible_child_name(GTK_STACK(chat_page), found ? "messages" : "welcome");
    gtk_widget_set_sensitive(folder, !found);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tab), FALSE);
    gtk_widget_grab_focus(editor);
    note(found ? "Session history" : "Ready");
}
static void list_history(void);
static void picked(GtkButton *w, gpointer data) {
    (void)w;
    select_thread(data);
    list_history();
}
static void list_history(void) {
    GList *items = gtk_container_get_children(GTK_CONTAINER(history));
    for (GList *i = items; i; i = i->next)
        gtk_widget_destroy(i->data);
    g_list_free(items);
    g_autofree char *query = g_utf8_casefold(gtk_entry_get_text(GTK_ENTRY(search)), -1);
    g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
    for (int i = (int)turns->len - 1; i >= 0; i--) {
        JsonObject *o = ((Turn *)g_ptr_array_index(turns, i))->data;
        const char *id = field(o, "thread");
        if (g_hash_table_contains(seen, id))
            continue;
        g_hash_table_add(seen, (gpointer)id);
        g_autofree char *folded = g_utf8_casefold(field(o, "title"), -1);
        if (!strstr(folded, query))
            continue;
        GtkWidget *w = button(field(o, "title"), !strcmp(id, thread_id) ? "selected" : "session", NULL);
        GtkLabel *caption = GTK_LABEL(gtk_bin_get_child(GTK_BIN(w)));
        gtk_label_set_xalign(caption, 0);
        gtk_label_set_ellipsize(caption, PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(caption, 19);
        gtk_widget_set_tooltip_text(w, field(o, "title"));
        pack(history, w, FALSE);
        g_signal_connect(w, "clicked", G_CALLBACK(picked), (gpointer)id);
    }
    gtk_widget_show_all(history);
}
static void filter_sessions(GtkEditable *unused, gpointer data) {
    (void)unused;
    (void)data;
    list_history();
}
static void create_thread(void) {
    g_autofree char *id = g_uuid_string_random();
    select_thread(id);
    list_history();
}
static void new_session(GtkButton *w, gpointer unused) {
    (void)w;
    (void)unused;
    create_thread();
}
static void controls(gboolean busy) {
    gtk_widget_set_sensitive(history, !busy);
    gtk_widget_set_sensitive(search, !busy);
    gtk_widget_set_sensitive(new_button, !busy);
    gtk_widget_set_sensitive(send_button, TRUE);
    gtk_button_set_label(GTK_BUTTON(send_button), busy ? "Stop  ■" : "Send  ↑");
    gtk_widget_set_sensitive(folder, !busy && !*thread_title);
}
static void receive(const char *line, gboolean is_stderr) {
    if (!active)
        return;
    g_autoptr(JsonParser) parser = json_parser_new();
    if (is_stderr || !json_parser_load_from_data(parser, line, -1, NULL) ||
        !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        if (*line) {
            g_autofree char *clean = g_utf8_make_valid(line, -1);
            g_string_append(diagnostic, clean);
            g_string_append_c(diagnostic, '\n');
            if (diagnostic->len > 8192) {
                char *cut = strchr(diagnostic->str + diagnostic->len - 8192, '\n');
                if (cut)
                    g_string_erase(diagnostic, 0, cut + 1 - diagnostic->str);
            }
        }
        return;
    }
    JsonObject *o = json_node_get_object(json_parser_get_root(parser));
    JsonArray *events = json_object_get_array_member(active->data, "events");
    if (!g_str_has_prefix(field(o, "type"), "preview_"))
        json_array_add_object_element(events, json_object_ref(o));
    render_event(o, TRUE);
}
static void completed(int code) {
    clear_preview();
    if (stop_file)
        remove(stop_file);
    g_clear_pointer(&stop_file, g_free);
    g_unsetenv("LIXINOS_DSH_STOP_FILE");
    if (!stop_accepted && (code || !final_seen)) {
        JsonObject *e = json_object_new();
        g_autofree char *s = g_strdup_printf("dsh exited with status %d. %s", code,
                                             diagnostic->len ? diagnostic->str
                                                             : "Check the engine configuration in DSH_HOME.");
        json_object_set_string_member(e, "type", "error");
        json_object_set_string_member(e, "message", s);
        json_array_add_object_element(json_object_get_array_member(active->data, "events"), e);
        render_event(e, FALSE);
    }
    note(stop_accepted ? "Stopped" : code || !final_seen ? "Turn failed" : "Complete");
    json_object_set_boolean_member(active->data, "closed", TRUE);
    save_turn(active);
    active = NULL;
    controls(FALSE);
    gtk_widget_grab_focus(editor);
}
static void submit(void) {
    if (dsh_busy())
        return;
    GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(editor));
    GtkTextIter first, last;
    gtk_text_buffer_get_bounds(b, &first, &last);
    g_autofree char *prompt = gtk_text_buffer_get_text(b, &first, &last, FALSE);
    if (prompt[strspn(prompt, " \t\r\n")] == 0)
        return;
    const char *cwd = gtk_entry_get_text(GTK_ENTRY(folder));
    if (!g_path_is_absolute(cwd) || !g_file_test(cwd, G_FILE_TEST_IS_DIR)) {
        note("Enter an existing absolute workspace path.");
        return;
    }
    if (g_mkdir_with_parents(archive, 0700) < 0) {
        note(g_strerror(errno));
        return;
    }
    g_autofree char *uuid = g_uuid_string_random();
    g_autofree char *name = g_strdup_printf("%019" G_GINT64_FORMAT "-%s.json", g_get_real_time(), uuid);
    Turn *turn = g_new0(Turn, 1);
    turn->file = g_build_filename(archive, name, NULL);
    turn->data = json_object_new();
    g_autofree char *short_title = g_utf8_substring(prompt, 0, MIN(48, g_utf8_strlen(prompt, -1)));
    g_strdelimit(short_title, "\r\n\t", ' ');
    const char *caption = *thread_title ? thread_title : short_title;
    json_object_set_string_member(turn->data, "title", caption);
    json_object_set_string_member(turn->data, "thread", thread_id);
    json_object_set_string_member(turn->data, "session", session_id);
    json_object_set_string_member(turn->data, "cwd", cwd);
    json_object_set_string_member(turn->data, "prompt", prompt);
    json_object_set_array_member(turn->data, "events", json_array_new());
    if (!save_turn(turn)) {
        release_turn(turn);
        return;
    }
    g_hash_table_remove_all(pending_searches);
    active = turn;
    g_ptr_array_add(turns, turn);
    replace(&thread_title, caption);
    final_seen = stop_accepted = FALSE;
    replace(&last_text, "");
    stop_file = g_strdup_printf("%s/stop-%s", archive, uuid);
    g_setenv("LIXINOS_DSH_STOP_FILE", stop_file, TRUE);
    g_string_truncate(diagnostic, 0);
    gtk_label_set_text(GTK_LABEL(title), thread_title);
    gtk_stack_set_visible_child_name(GTK_STACK(chat_page), "messages");
    gtk_text_buffer_set_text(b, "", 0);
    message(chat, "You", prompt, NULL);
    trace_message(USER, "User", prompt, NULL);
    list_history();
    controls(TRUE);
    note("Starting DeepSeek Harness…");
    int error = dsh_start(cwd, session_id, prompt, receive, completed);
    if (error) {
        g_string_assign(diagnostic, g_strerror(error));
        completed(error);
    }
}
static void send_clicked(GtkButton *w, gpointer unused) {
    (void)unused;
    if (!dsh_busy()) {
        submit();
        return;
    }
    g_autoptr(GError) error = NULL;
    if (!g_file_set_contents(stop_file, "stop", 4, &error)) {
        note(error->message);
        return;
    }
    gtk_widget_set_sensitive(GTK_WIDGET(w), FALSE);
    note("Stop requested…");
}
static gboolean keypress(GtkWidget *w, GdkEventKey *key, gpointer unused) {
    (void)w;
    (void)unused;
    if ((key->keyval == GDK_KEY_s || key->keyval == GDK_KEY_Return) && (key->state & GDK_CONTROL_MASK)) {
        submit();
        return TRUE;
    }
    return FALSE;
}
static void show_trace(GtkToggleButton *w, gpointer unused) {
    (void)unused;
    gtk_stack_set_visible_child_name(GTK_STACK(pages), gtk_toggle_button_get_active(w) ? "trace" : "chat");
}
static void toggle_side(GtkButton *w, gpointer unused) {
    (void)w;
    (void)unused;
    gboolean show = !gtk_widget_get_visible(sidebar);
    gtk_widget_set_visible(sidebar, show);
    gtk_button_set_label(w, show ? "‹" : "›");
}
static gboolean closing(GtkWidget *w, GdkEvent *event, gpointer unused) {
    (void)w;
    (void)event;
    (void)unused;
    if (dsh_busy())
        note("Use Stop before closing this window.");
    return dsh_busy();
}
static void add_file(GtkButton *w, gpointer unused) {
    (void)w;
    (void)unused;
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Reference a workspace file", GTK_WINDOW(window),
                                                 GTK_FILE_CHOOSER_ACTION_OPEN, "Cancel", GTK_RESPONSE_CANCEL,
                                                 "Reference", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), gtk_entry_get_text(GTK_ENTRY(folder)));
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        g_autofree char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (path) {
            g_autofree char *ref = g_strdup_printf(" %s ", path);
            gtk_text_buffer_insert_at_cursor(gtk_text_view_get_buffer(GTK_TEXT_VIEW(editor)), ref, -1);
        }
    }
    gtk_widget_destroy(dlg);
    gtk_widget_grab_focus(editor);
}
static const char css[] =
    "* {font-family:'Fira Sans','Noto Sans Mono CJK SC',sans-serif;font-size:13px;color:#24272f;}"
    "window,stack, textview, textview text,scrolledwindow {background:#ffffff;}"
    "tooltip {background:#283347;} tooltip label {color:#ffffff;}"
    "#chat-transcript, #chat-transcript text {font-size:14px;}"
    "button {background-image:none;background-color:transparent;border:0;box-shadow:none;"
    "border-radius:7px;padding:6px 10px;} button:hover {background-color:#e9edf4;}"
    "#sidebar,#sidebar scrolledwindow,#sidebar viewport,#history {background:#f5f6f8;}"
    "#sidebar {border-right:1px solid #e4e6eb;padding:12px 9px;}"
    "#brand {font-size:15px;font-weight:bold;} #section,#status {font-size:11px;color:#687184;}"
    "#search {min-height:20px;padding:2px 6px;border:0;background:#ebedf2;box-shadow:none;}"
    "#new {background:#e6edff;color:#315db5;font-weight:bold;}"
    "#selected {background:#e3e8f0;} #header {padding:6px;border-bottom:1px solid #edf0f4;}"
    "#title {font-size:14px;font-weight:bold;} #welcome-title {font-size:23px;font-weight:bold;}"
    "#workspace "
    "{border:0;box-shadow:none;background:transparent;color:#58657a;padding:2px;min-height:18px;}"
    "#trace-header {background:#20242b;padding:5px 14px;} #trace-count {color:#d5deed;font-size:11px;}"
    "#trace-console, #trace-console text {background:#191c22;color:#e9edf7;font-family:monospace;}"
    "#composer {border:1px solid #d9dee7;border-radius:12px;padding:8px;background:#ffffff;}"
    "#send {background:#416ce1;color:#ffffff;font-weight:bold;} #send:disabled {background:#91a1c4;}"
    "#send label {color:white;} #trace:checked {background:#e8edff;color:#315db5;} #welcome {padding:16px;}";
static void editor_changed(GtkTextBuffer *buffer, gpointer unused) {
    (void)unused;
    gtk_widget_set_visible(placeholder, gtk_text_buffer_get_char_count(buffer) == 0);
}
static void connection(GtkButton *w, gpointer unused) {
    (void)w;
    (void)unused;
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Model connection", GTK_WINDOW(window), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "Cancel",
        GTK_RESPONSE_CANCEL, "Apply", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(box), 14);
    gtk_box_set_spacing(GTK_BOX(box), 8);
    pack(box, label("API endpoint", "brand"), FALSE);
    GtkWidget *url = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(url), 40);
    const char *current = g_getenv("DEEPSEEK_BASE_URL");
    gtk_entry_set_text(GTK_ENTRY(url), current ? current : "");
    gtk_entry_set_placeholder_text(GTK_ENTRY(url), "Use engine configuration");
    pack(box, url, FALSE);
    pack(box, label("API key for this window", "brand"), FALSE);
    GtkWidget *key = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(key), FALSE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(key), "Leave blank to keep the configured key");
    pack(box, key, FALSE);
    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        const char *endpoint = gtk_entry_get_text(GTK_ENTRY(url));
        const char *secret = gtk_entry_get_text(GTK_ENTRY(key));
        if (*endpoint && !(g_str_has_prefix(endpoint, "https://") || g_str_has_prefix(endpoint, "http://"))) {
            note("Use an HTTP or HTTPS API endpoint.");
        } else {
            if (*endpoint)
                g_setenv("DEEPSEEK_BASE_URL", endpoint, TRUE);
            else
                g_unsetenv("DEEPSEEK_BASE_URL");
            if (*secret)
                g_setenv("DEEPSEEK_API_KEY", secret, TRUE);
            note("Connection settings apply to the next turn.");
        }
    }
    gtk_entry_set_text(GTK_ENTRY(key), "");
    gtk_widget_destroy(dialog);
}
static void build_window(const char *initial_cwd) {
    GtkCssProvider *style = gtk_css_provider_new();
    gtk_css_provider_load_from_data(style, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(style),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(style);
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "DeepSeek Harness");
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(window), 720, 436);
    g_signal_connect(window, "delete-event", G_CALLBACK(closing), NULL);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(window), root);
    sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_name(sidebar, "sidebar");
    gtk_widget_set_size_request(sidebar, 154, -1);
    pack(root, sidebar, FALSE);
    pack(sidebar, label("DeepSeek Harness", "brand"), FALSE);
    new_button = button("＋  New session", "new", G_CALLBACK(new_session));
    pack(sidebar, new_button, FALSE);
    search = gtk_search_entry_new();
    gtk_widget_set_name(search, "search");
    gtk_entry_set_width_chars(GTK_ENTRY(search), 12);
    gtk_entry_set_placeholder_text(GTK_ENTRY(search), "Find sessions");
    g_signal_connect(search, "changed", G_CALLBACK(filter_sessions), NULL);
    pack(sidebar, search, FALSE);
    GtkWidget *list_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(list_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    history = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_name(history, "history");
    gtk_container_add(GTK_CONTAINER(list_scroll), history);
    pack(sidebar, list_scroll, TRUE);
    pack(sidebar, button("Model connection", "connection", G_CALLBACK(connection)), FALSE);

    GtkWidget *main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    pack(root, main, TRUE);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_name(header, "header");
    pack(main, header, FALSE);
    GtkWidget *toggle = button("‹", "toggle", G_CALLBACK(toggle_side));
    gtk_widget_set_tooltip_text(toggle, "Show or hide sessions");
    pack(header, toggle, FALSE);
    title = label("New session", "title");
    pack(header, title, TRUE);
    tab = gtk_toggle_button_new_with_label("Trajectory");
    gtk_widget_set_name(tab, "trace");
    pack(header, tab, FALSE);
    g_signal_connect(tab, "toggled", G_CALLBACK(show_trace), NULL);
    pages = gtk_stack_new();
    pack(main, pages, TRUE);
    chat_page = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(pages), chat_page, "chat");
    welcome = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_name(welcome, "welcome");
    gtk_widget_set_valign(welcome, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(welcome, GTK_ALIGN_CENTER);
    pack(welcome, gtk_image_new_from_file("/lib/dsh-gui/deepseek.png"), FALSE);
    pack(welcome, label("DeepSeek Harness", "welcome-title"), FALSE);
    gtk_stack_add_named(GTK_STACK(chat_page), welcome, "welcome");
    chat_links = g_ptr_array_new();
    trace_links = g_ptr_array_new();
    preview_links = g_ptr_array_new();
    preview_source = g_string_new(NULL);
    GtkWidget *messages = transcript(&chat);
    gtk_stack_add_named(GTK_STACK(chat_page), messages, "messages");
    GtkWidget *trace_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *trace_header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(trace_header, "trace-header");
    trace_count = label("", "trace-count");
    pack(trace_header, trace_count, FALSE);
    timeline = gtk_drawing_area_new();
    gtk_widget_set_size_request(timeline, -1, 16);
    gtk_widget_set_tooltip_text(timeline, "Click an event to jump to its details");
    gtk_widget_add_events(timeline, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(timeline, "draw", G_CALLBACK(draw_timeline), NULL);
    g_signal_connect(timeline, "button-press-event", G_CALLBACK(jump_to_event), NULL);
    pack(trace_header, timeline, FALSE);
    pack(trace_page, trace_header, FALSE);
    pack(trace_page, transcript(&trace), TRUE);
    gtk_stack_add_named(GTK_STACK(pages), trace_page, "trace");

    GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(bottom), 10);
    pack(main, bottom, FALSE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_name(box, "composer");
    pack(bottom, box, FALSE);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 44);
    editor = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(editor), GTK_WRAP_WORD_CHAR);
    gtk_widget_set_tooltip_text(editor, "Message DeepSeek · Ctrl+S to send, Enter for a new line");
    gtk_container_add(GTK_CONTAINER(scroll), editor);
    GtkWidget *overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(overlay), scroll);
    placeholder = label("Message DeepSeek…", "section");
    gtk_widget_set_halign(placeholder, GTK_ALIGN_START);
    gtk_widget_set_valign(placeholder, GTK_ALIGN_START);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), placeholder);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(overlay), placeholder, TRUE);
    g_signal_connect(gtk_text_view_get_buffer(GTK_TEXT_VIEW(editor)), "changed", G_CALLBACK(editor_changed),
                     NULL);
    pack(box, overlay, FALSE);
    g_signal_connect(editor, "key-press-event", G_CALLBACK(keypress), NULL);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    pack(box, bar, FALSE);
    GtkWidget *attach = button("＋", "attach", G_CALLBACK(add_file));
    gtk_widget_set_tooltip_text(attach, "Reference a file");
    pack(bar, attach, FALSE);
    pack(bar, label("Workspace", "section"), FALSE);
    folder = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(folder), 9);
    gtk_widget_set_name(folder, "workspace");
    gtk_entry_set_text(GTK_ENTRY(folder), initial_cwd);
    gtk_widget_set_tooltip_text(folder, "Working directory for this session");
    pack(bar, folder, TRUE);
    send_button = button("Send  ↑", "send", G_CALLBACK(send_clicked));
    pack(bar, send_button, FALSE);
    status = label("Ready", "status");
    pack(bottom, status, FALSE);
    gtk_widget_show_all(window);
}
int main(int argc, char **argv) {
    g_autofree char *cwd = g_get_current_dir();
    g_autofree char *start = g_canonicalize_filename(argc > 1 ? argv[1] : cwd, cwd);
    const char *env = g_getenv("DSH_HOME");
    g_autofree char *home =
        env && *env ? g_canonicalize_filename(env, cwd) : g_build_filename(g_get_home_dir(), ".dsh", NULL);
    g_setenv("DSH_HOME", home, TRUE);
    archive = g_build_filename(home, "lixinos-gui", "turns", NULL);
    g_setenv("GDK_BACKEND", "broadway", TRUE);
    g_setenv("GSETTINGS_BACKEND", "memory", TRUE);
    g_setenv("NO_AT_BRIDGE", "1", TRUE);
    g_setenv("FONTCONFIG_FILE", "/lib/gtk3/fonts.conf", TRUE);
    if (!gtk_init_check(&argc, &argv))
        return 1;
    diagnostic = g_string_new(NULL);
    pending_searches = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    turns = g_ptr_array_new_with_free_func(release_turn);
    trace_points = g_array_new(FALSE, FALSE, sizeof(TracePoint));
    load_turns();
    build_window(start);
    create_thread();
    gtk_main();
    return 0;
}
