#include "markdown.h"
#include <md4c.h>
#include <entity.h>
#include <string.h>

extern gboolean gdk_lixinos_open_web_uri(const char *uri);

typedef struct {
    int start;
    const char *style;
    GtkTextTag *tag;
    guint next_item;
    gboolean ordered;
} Frame;

typedef struct {
    GtkTextBuffer *buffer;
    GtkTextIter end;
    GArray *stack;
    GPtrArray *dynamic_tags;
    guint depth, column;
    GArray *widths;
    PangoLayout *measure;
} Render;

static void put(Render *r, const char *text, int len) {
    gtk_text_buffer_insert(r->buffer, &r->end, text, len);
}
static void end_line(Render *r) {
    if (!gtk_text_iter_starts_line(&r->end))
        put(r, "\n", 1);
}
static void pop(Render *r) {
    Frame f = g_array_index(r->stack, Frame, r->stack->len - 1);
    g_array_set_size(r->stack, r->stack->len - 1);
    GtkTextIter start;
    gtk_text_buffer_get_iter_at_offset(r->buffer, &start, f.start);
    if (f.style)
        gtk_text_buffer_apply_tag_by_name(r->buffer, f.style, &start, &r->end);
    if (f.tag)
        gtk_text_buffer_apply_tag(r->buffer, f.tag, &start, &r->end);
}
static const char *headings[] = {"md-h1", "md-h2", "md-h3", "md-h4", "md-h5", "md-h6"};
static const char *indents[] = {"md-li1", "md-li2", "md-li3", "md-li4"};
static int block_start(MD_BLOCKTYPE type, void *detail, void *data) {
    Render *r = data;
    Frame f = {.start = gtk_text_iter_get_offset(&r->end)};
    switch (type) {
    case MD_BLOCK_H:
        f.style = headings[((MD_BLOCK_H_DETAIL *)detail)->level - 1];
        break;
    case MD_BLOCK_P: f.style = "md-p"; break;
    case MD_BLOCK_QUOTE: f.style = "md-quote"; break;
    case MD_BLOCK_CODE: f.style = "md-fence"; break;
    case MD_BLOCK_TABLE: {
        guint columns = ((MD_BLOCK_TABLE_DETAIL *)detail)->col_count;
        r->widths = g_array_sized_new(FALSE, TRUE, sizeof(int), columns);
        g_array_set_size(r->widths, columns);
        GtkWidget *view = g_object_get_data(G_OBJECT(r->buffer), "view");
        r->measure = gtk_widget_create_pango_layout(view, NULL);
        break;
    }
    case MD_BLOCK_TR:
        r->column = 0;
        break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        if (r->column) put(r, "\t", 1);
        f.start = gtk_text_iter_get_offset(&r->end);
        if (type == MD_BLOCK_TH) f.style = "md-strong";
        break;
    case MD_BLOCK_HR: put(r, "────────────", -1); break;
    case MD_BLOCK_OL:
    case MD_BLOCK_UL:
        end_line(r);
        r->depth++;
        if (type == MD_BLOCK_OL) {
            f.ordered = TRUE;
            f.next_item = ((MD_BLOCK_OL_DETAIL *)detail)->start;
        }
        break;
    case MD_BLOCK_LI: {
        Frame *list = &g_array_index(r->stack, Frame, r->stack->len - 1);
        f.style = indents[MIN(r->depth, G_N_ELEMENTS(indents)) - 1];
        if (list->ordered) {
            char marker[24];
            g_snprintf(marker, sizeof marker, "%u.  ", list->next_item++);
            put(r, marker, -1);
        } else {
            put(r, "•  ", -1);
        }
        break;
    }
    default: break;
    }
    g_array_append_val(r->stack, f);
    return 0;
}
static int block_end(MD_BLOCKTYPE type, void *detail, void *data) {
    (void)detail;
    Render *r = data;
    if (type == MD_BLOCK_UL || type == MD_BLOCK_OL)
        r->depth--;
    if (type == MD_BLOCK_TD || type == MD_BLOCK_TH) {
        Frame cell = g_array_index(r->stack, Frame, r->stack->len - 1);
        GtkTextIter from;
        gtk_text_buffer_get_iter_at_offset(r->buffer, &from, cell.start);
        g_autofree char *value = gtk_text_iter_get_text(&from, &r->end);
        pango_layout_set_text(r->measure, value, -1);
        int width;
        pango_layout_get_pixel_size(r->measure, &width, NULL);
        int *maximum = &g_array_index(r->widths, int, r->column++);
        *maximum = MAX(*maximum, width);
    } else {
        end_line(r);
    }
    if (type == MD_BLOCK_TABLE) {
        PangoTabArray *tabs = pango_tab_array_new(r->widths->len, TRUE);
        int x = 0;
        for (guint i = 0; i < r->widths->len; i++) {
            x += g_array_index(r->widths, int, i) + 24;
            pango_tab_array_set_tab(tabs, i, PANGO_TAB_LEFT, x);
        }
        GtkTextTag *tag = gtk_text_buffer_create_tag(r->buffer, NULL, "tabs", tabs, NULL);
        g_ptr_array_add(r->dynamic_tags, tag);
        g_array_index(r->stack, Frame, r->stack->len - 1).tag = tag;
        pango_tab_array_free(tabs);
        g_clear_pointer(&r->widths, g_array_unref);
        g_clear_object(&r->measure);
    }
    pop(r);
    return 0;
}

static void entity(GString *out, const char *s, guint n) {
    if (n > 3 && s[1] == '#') {
        gboolean hex = s[2] == 'x' || s[2] == 'X';
        guint64 cp = g_ascii_strtoull(s + (hex ? 3 : 2), NULL, hex ? 16 : 10);
        g_string_append_unichar(out, cp && cp <= 0x10ffff && g_unichar_validate(cp) ? cp : 0xfffd);
        return;
    }
    const ENTITY *value = entity_lookup(s, n);
    if (!value) {
        g_string_append_len(out, s, n);
        return;
    }
    g_string_append_unichar(out, value->codepoints[0]);
    if (value->codepoints[1]) g_string_append_unichar(out, value->codepoints[1]);
}

static char *normalize_url(const char *raw) {
    if (g_ascii_strncasecmp(raw, "http://", 7) && g_ascii_strncasecmp(raw, "https://", 8))
        return NULL;
    g_autoptr(GUri) uri = g_uri_parse(raw, G_URI_FLAGS_ENCODED, NULL);
    if (!uri || !g_uri_get_host(uri) || !*g_uri_get_host(uri)) return NULL;
    return g_uri_to_string(uri);
}

static GtkTextTag *link_tag(GtkTextBuffer *buffer, GPtrArray *tags, char *url, const char *color) {
    GtkTextTag *tag = gtk_text_buffer_create_tag(buffer, NULL, "foreground", color,
                                                  "underline", PANGO_UNDERLINE_SINGLE, NULL);
    g_object_set_data_full(G_OBJECT(tag), "href", url, g_free);
    g_ptr_array_add(tags, tag);
    return tag;
}

static char *web_uri(const MD_ATTRIBUTE *attribute) {
    GString *url = g_string_new(NULL);
    for (guint i = 0; attribute->substr_offsets && attribute->substr_offsets[i] < attribute->size; i++) {
        guint offset = attribute->substr_offsets[i];
        guint n = attribute->substr_offsets[i + 1] - offset;
        if (attribute->substr_types[i] == MD_TEXT_ENTITY)
            entity(url, attribute->text + offset, n);
        else
            g_string_append_len(url, attribute->text + offset, n);
    }
    g_autofree char *raw = g_string_free(url, FALSE);
    return normalize_url(raw);
}

static int span_start(MD_SPANTYPE type, void *detail, void *data) {
    Render *r = data;
    Frame f = {.start = gtk_text_iter_get_offset(&r->end)};
    switch (type) {
    case MD_SPAN_STRONG: f.style = "md-strong"; break;
    case MD_SPAN_EM: f.style = "md-em"; break;
    case MD_SPAN_DEL: f.style = "md-del"; break;
    case MD_SPAN_CODE: f.style = "md-code"; break;
    case MD_SPAN_A: {
        char *url = web_uri(&((MD_SPAN_A_DETAIL *)detail)->href);
        if (url) f.tag = link_tag(r->buffer, r->dynamic_tags, url, "#245bc2");
        break;
    }
    default: break;
    }
    g_array_append_val(r->stack, f);
    return 0;
}
static int span_end(MD_SPANTYPE type, void *detail, void *data) {
    (void)type; (void)detail;
    pop(data);
    return 0;
}
static int text(MD_TEXTTYPE type, const char *s, MD_SIZE size, void *data) {
    Render *r = data;
    if (type == MD_TEXT_ENTITY) {
        GString *decoded = g_string_new(NULL);
        entity(decoded, s, size);
        put(r, decoded->str, decoded->len);
        g_string_free(decoded, TRUE);
    } else if (type == MD_TEXT_NULLCHAR) {
        put(r, "�", -1);
    } else if (type == MD_TEXT_BR || type == MD_TEXT_SOFTBR) {
        put(r, type == MD_TEXT_BR ? "\n" : " ", 1);
    } else {
        put(r, s, size);
    }
    return 0;
}
void markdown_append(GtkTextBuffer *buffer, const char *source, GPtrArray *links) {
    MD_PARSER parser = {.flags = MD_FLAG_NOHTML | MD_FLAG_PERMISSIVEURLAUTOLINKS | MD_FLAG_STRIKETHROUGH | MD_FLAG_TABLES,
                        .enter_block = block_start, .leave_block = block_end, .enter_span = span_start,
                        .leave_span = span_end, .text = text};
    Render render = {.buffer = buffer, .stack = g_array_new(FALSE, FALSE, sizeof(Frame)), .dynamic_tags = links};
    gtk_text_buffer_get_end_iter(buffer, &render.end);
    md_parse(source, strlen(source), &parser, &render);
    g_array_free(render.stack, TRUE);
    g_clear_pointer(&render.widths, g_array_unref);
    g_clear_object(&render.measure);
}
/* Keep raw diagnostic text intact while making the URLs in search results usable. */
void markdown_linkify(GtkTextBuffer *buffer, int start, const char *source, GPtrArray *tags, gboolean dark) {
    static GRegex *pattern;
    if (!pattern)
        pattern = g_regex_new("https?://[^[:space:]<>\"\\\\`，。；、（）【】《》]+",
                              G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, NULL);
    GMatchInfo *matches;
    g_regex_match(pattern, source, 0, &matches);
    int previous = 0, offset = start;
    while (g_match_info_matches(matches)) {
        int a, b;
        g_match_info_fetch_pos(matches, 0, &a, &b);
        int parentheses = 0;
        for (int i = a; i < b; i++) {
            if (source[i] == '(') parentheses++;
            else if (source[i] == ')' && --parentheses < 0) { b = i; break; }
        }
        while (b > a && strchr(".,;:!?]}", source[b-1])) b--;
        g_autofree char *raw = g_strndup(source+a, b-a);
        char *url = normalize_url(raw);
        offset += g_utf8_strlen(source+previous, a-previous);
        previous = a;
        if (url) {
            GtkTextIter first, last;
            gtk_text_buffer_get_iter_at_offset(buffer, &first, offset);
            gtk_text_buffer_get_iter_at_offset(buffer, &last, offset+g_utf8_strlen(raw, -1));
            gtk_text_buffer_apply_tag(buffer, link_tag(buffer, tags, url, dark ? "#99bcff" : "#245bc2"),
                                      &first, &last);
        }
        g_match_info_next(matches, NULL);
    }
    g_match_info_free(matches);
}

void markdown_clear_links(GtkTextBuffer *buffer, GPtrArray *links) {
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
    for (guint i = 0; i < links->len; i++)
        gtk_text_tag_table_remove(table, g_ptr_array_index(links, i));
    g_ptr_array_set_size(links, 0);
}
static const char *link_at(GtkTextView *view, int x, int y) {
    GtkTextIter iter;
    gtk_text_view_window_to_buffer_coords(view, GTK_TEXT_WINDOW_TEXT, x, y, &x, &y);
    if (!gtk_text_view_get_iter_at_position(view, &iter, NULL, x, y)) return NULL;
    GSList *tags = gtk_text_iter_get_tags(&iter);
    const char *url = NULL;
    for (GSList *p = tags; p && !url; p = p->next)
        url = g_object_get_data(p->data, "href");
    g_slist_free(tags);
    return url;
}
static gboolean click(GtkTextView *view, GdkEventButton *event, gpointer unused) {
    (void)unused;
    if (event->button != 1 || event->window != gtk_text_view_get_window(view, GTK_TEXT_WINDOW_TEXT))
        return FALSE;
    const char *url = link_at(view, event->x, event->y);
    if (event->type == GDK_BUTTON_PRESS) {
        g_object_set_data_full(G_OBJECT(view), "pressed-link", g_strdup(url), g_free);
        return FALSE;
    }
    gboolean activate = url && !g_strcmp0(url, g_object_get_data(G_OBJECT(view), "pressed-link")) &&
        !gtk_text_buffer_get_has_selection(gtk_text_view_get_buffer(view));
    g_object_set_data(G_OBJECT(view), "pressed-link", NULL);
    if (activate && !gdk_lixinos_open_web_uri(url)) {
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(view))),
            GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE, "Web address");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", url);
        g_signal_connect_swapped(dialog, "response", G_CALLBACK(gtk_widget_destroy), dialog);
        gtk_widget_show(dialog);
    }
    /* GtkTextView must see the release to end its selection gesture before
     * the desktop gives focus to the browser. */
    return FALSE;
}
static gboolean tooltip(GtkWidget *view, gint x, gint y, gboolean keyboard, GtkTooltip *tip, gpointer unused) {
    (void)unused;
    if (keyboard) return FALSE;
    const char *url = link_at(GTK_TEXT_VIEW(view), x, y);
    if (url) gtk_tooltip_set_text(tip, url);
    return url != NULL;
}
void markdown_setup(GtkTextView *view) {
    GtkTextBuffer *b = gtk_text_view_get_buffer(view);
    for (guint i = 0; i < G_N_ELEMENTS(headings); i++)
        gtk_text_buffer_create_tag(b, headings[i], "scale", i == 0 ? 1.42 : i == 1 ? 1.26 : 1.10,
            "weight", PANGO_WEIGHT_BOLD, "pixels-above-lines", 8, "pixels-below-lines", 4, NULL);
    for (guint i = 0; i < G_N_ELEMENTS(indents); i++)
        gtk_text_buffer_create_tag(b, indents[i], "left-margin", 36 + 18 * (int)i, "indent", -15, NULL);
    gtk_text_buffer_create_tag(b, "md-p", "pixels-below-lines", 6, NULL);
    gtk_text_buffer_create_tag(b, "md-quote", "left-margin", 32, "foreground", "#586477",
                               "paragraph-background", "#f3f5f9", NULL);
    gtk_text_buffer_create_tag(b, "md-fence", "family", "monospace", "paragraph-background", "#f0f3f8",
                               "left-margin", 27, NULL);
    gtk_text_buffer_create_tag(b, "md-strong", "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(b, "md-em", "style", PANGO_STYLE_ITALIC, NULL);
    gtk_text_buffer_create_tag(b, "md-del", "strikethrough", TRUE, NULL);
    gtk_text_buffer_create_tag(b, "md-code", "family", "monospace", "background", "#edf0f6", NULL);
}
void markdown_enable_links(GtkTextView *view) {
    gtk_widget_set_has_tooltip(GTK_WIDGET(view), TRUE);
    g_signal_connect(view, "query-tooltip", G_CALLBACK(tooltip), NULL);
    g_signal_connect(view, "button-press-event", G_CALLBACK(click), NULL);
    g_signal_connect(view, "button-release-event", G_CALLBACK(click), NULL);
}
