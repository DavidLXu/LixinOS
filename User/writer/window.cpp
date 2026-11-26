/* Native Win3 frontend for Writer's LibreOfficeKit document model. */
#define LOK_USE_UNSTABLE_API
#include <gtk/gtk.h>
#include <LibreOfficeKit/LibreOfficeKit.h>
#include <LibreOfficeKit/LibreOfficeKitEnums.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" LibreOfficeKit *libreofficekit_hook_2(const char *, const char *);
static LibreOfficeKit *office;
static LibreOfficeKitDocument *doc;
static GtkWidget *window, *canvas, *status, *format_bar;
static GtkWidget *style_combo, *font_combo, *size_combo;
static GtkWidget *bold_button, *italic_button, *underline_button;
static GtkWidget *align_buttons[4];
static GtkWidget *bullet_button, *color_button;
static GtkWidget *find_bar, *replace_row, *find_entry, *replace_entry;
static GtkAdjustment *scroll;
static char *filename;
static gboolean dirty;
static int syncing;
static gboolean combo_typing;
static guint combo_typing_timeout;
static double units = 15;
static int caret_x, caret_y, caret_height, redraw_pending;

static char *json_quote(const char *text);
static void show_find(gboolean replace);

static void set_dirty(gboolean value) {
    dirty = value;
    char *base = filename ? g_path_get_basename(filename) : g_strdup("未命名");
    char *title = g_strdup_printf("%s%s — LibreOffice Writer", base, dirty ? " *" : "");
    gtk_window_set_title(GTK_WINDOW(window), title);
    g_free(title);
    g_free(base);
}

static gboolean repaint(gpointer unused) {
    g_atomic_int_set(&redraw_pending, 0);
    if (canvas) gtk_widget_queue_draw(canvas);
    return G_SOURCE_REMOVE;
}

static const char *state_value(const char *payload, const char *name) {
    size_t length = strlen(name);
    return !strncmp(payload, name, length) && payload[length] == '=' ? payload + length + 1 : NULL;
}

static GtkWidget *combo_entry(GtkWidget *combo) {
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(combo));
    return GTK_IS_ENTRY(child) ? child : NULL;
}

static void combo_set_text(GtkWidget *combo, const char *text) {
    GtkWidget *entry = combo_entry(combo);
    if (entry) gtk_entry_set_text(GTK_ENTRY(entry), text);
}

static const char *combo_text(GtkWidget *combo) {
    GtkWidget *entry = combo_entry(combo);
    return entry ? gtk_entry_get_text(GTK_ENTRY(entry)) : "";
}

/* Typing in an editable combo makes GtkComboBox emit "changed" for every
 * character.  Mark those key presses so the handler can wait for Enter or a
 * mouse selection instead of applying a half-typed font name. */
static gboolean clear_combo_typing(gpointer unused) {
    combo_typing = FALSE;
    combo_typing_timeout = 0;
    return G_SOURCE_REMOVE;
}

static gboolean combo_key_press(GtkWidget *widget, GdkEventKey *event, gpointer unused) {
    if (event->keyval != GDK_KEY_Return && event->keyval != GDK_KEY_KP_Enter &&
        !(event->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK))) {
        combo_typing = TRUE;
        if (combo_typing_timeout) g_source_remove(combo_typing_timeout);
        combo_typing_timeout = g_timeout_add(500, clear_combo_typing, NULL);
    }
    return FALSE;
}

static void toggle_set(GtkWidget *button, gboolean active) {
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button)) != active)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), active);
}

static void set_align(int index, gboolean active) {
    if (active)
        for (int i = 0; i < 4; ++i)
            if (i != index) toggle_set(align_buttons[i], FALSE);
    toggle_set(align_buttons[index], active);
}

/* Writer's UI names are localized, while the document stores the English
 * style name.  The toolbar shows Chinese labels and posts English values. */
static const struct { const char *label, *value; } styles[] = {
    {"正文", "Text body"}, {"引用", "Quotations"}, {"标题", "Title"}, {"副标题", "Subtitle"},
    {"标题 1", "Heading 1"}, {"标题 2", "Heading 2"}, {"标题 3", "Heading 3"},
};

static const char *style_value(const char *label) {
    for (unsigned i = 0; i < G_N_ELEMENTS(styles); ++i)
        if (!g_strcmp0(label, styles[i].label) || !g_strcmp0(label, styles[i].value))
            return styles[i].value;
    return label;
}

static const char *style_label(const char *value) {
    if (!g_strcmp0(value, "Default Paragraph Style")) return "正文";
    for (unsigned i = 0; i < G_N_ELEMENTS(styles); ++i)
        if (!g_strcmp0(value, styles[i].value) || !g_strcmp0(value, styles[i].label))
            return styles[i].label;
    return value;
}

static void apply_format(const char *command, const char *arguments) {
    if (!doc) return;
    doc->pClass->postUnoCommand(doc, command, arguments, false);
    set_dirty(TRUE);
    gtk_widget_queue_draw(canvas);
}

static void apply_style(const char *value) {
    if (!doc || !value || !*value) return;
    char *label = json_quote(value);
    char *arguments = g_strdup_printf(
        "{\"Style\":{\"type\":\"string\",\"value\":%s},"
        "\"FamilyName\":{\"type\":\"string\",\"value\":\"ParagraphStyles\"}}", label);
    apply_format(".uno:StyleApply", arguments);
    g_free(arguments);
    g_free(label);
}

static void apply_font(const char *name) {
    if (!doc || !name || !*name) return;
    char *escaped = json_quote(name);
    char *arguments = g_strdup_printf(
        "{\"CharFontName.FamilyName\":{\"type\":\"string\",\"value\":%s}}", escaped);
    apply_format(".uno:CharFontName", arguments);
    g_free(arguments);
    g_free(escaped);
}

static void apply_size(const char *text) {
    char *end = NULL;
    double points = text ? g_ascii_strtod(text, &end) : 0;
    if (!doc || !end || end == text || points < 2 || points > 400) return;
    char *arguments = g_strdup_printf(
        "{\"FontHeight.Height\":{\"type\":\"float\",\"value\":\"%g\"}}", points);
    apply_format(".uno:FontHeight", arguments);
    g_free(arguments);
}

static void update_state(const char *payload) {
    const char *value = state_value(payload, ".uno:Bold");
    if (value) { syncing++; toggle_set(bold_button, !strcmp(value, "true")); syncing--; }
    if ((value = state_value(payload, ".uno:Italic"))) {
        syncing++; toggle_set(italic_button, !strcmp(value, "true")); syncing--;
    }
    if ((value = state_value(payload, ".uno:Underline"))) {
        syncing++; toggle_set(underline_button, !strcmp(value, "true")); syncing--;
    }
    if ((value = state_value(payload, ".uno:LeftPara")) ||
        (value = state_value(payload, ".uno:AlignLeft"))) {
        syncing++; set_align(0, !strcmp(value, "true")); syncing--;
    }
    if ((value = state_value(payload, ".uno:CenterPara")) ||
        (value = state_value(payload, ".uno:AlignHorizontalCenter"))) {
        syncing++; set_align(1, !strcmp(value, "true")); syncing--;
    }
    if ((value = state_value(payload, ".uno:RightPara")) ||
        (value = state_value(payload, ".uno:AlignRight"))) {
        syncing++; set_align(2, !strcmp(value, "true")); syncing--;
    }
    if ((value = state_value(payload, ".uno:JustifyPara"))) {
        syncing++; set_align(3, !strcmp(value, "true")); syncing--;
    }
    if ((value = state_value(payload, ".uno:DefaultBullet"))) {
        syncing++; toggle_set(bullet_button, !strcmp(value, "true")); syncing--;
    }
    if ((value = state_value(payload, ".uno:CharFontName"))) {
        syncing++; combo_set_text(font_combo, value); syncing--;
    }
    if ((value = state_value(payload, ".uno:FontHeight")) && *value) {
        syncing++; combo_set_text(size_combo, value); syncing--;
    }
    if ((value = state_value(payload, ".uno:StyleApply"))) {
        syncing++; combo_set_text(style_combo, style_label(value)); syncing--;
    }
    if ((value = state_value(payload, ".uno:Color"))) {
        long color = strtol(value, NULL, 10);
        if (color >= 0) {
            GdkRGBA rgba = {((color >> 16) & 255) / 255.0, ((color >> 8) & 255) / 255.0,
                            (color & 255) / 255.0, 1};
            gtk_color_button_set_rgba(GTK_COLOR_BUTTON(color_button), &rgba);
        }
    }
}

static void notification(int type, const char *payload, void *unused) {
    if (type == LOK_CALLBACK_STATE_CHANGED && payload) update_state(payload);
    if (type == LOK_CALLBACK_SEARCH_NOT_FOUND) {
        char *message = g_strdup_printf("未找到：%s", payload ? payload : "");
        gtk_label_set_text(GTK_LABEL(status), message);
        g_free(message);
    }
    if (type == LOK_CALLBACK_INVALIDATE_TILES || type == LOK_CALLBACK_DOCUMENT_SIZE_CHANGED ||
        type == LOK_CALLBACK_INVALIDATE_VISIBLE_CURSOR || type == LOK_CALLBACK_TEXT_SELECTION) {
        if (type == LOK_CALLBACK_INVALIDATE_VISIBLE_CURSOR && payload) {
            int x, y, w, h;
            if (sscanf(payload, "%d, %d, %d, %d", &x, &y, &w, &h) == 4) {
                g_atomic_int_set(&caret_x, x);
                g_atomic_int_set(&caret_y, y);
                g_atomic_int_set(&caret_height, h);
            }
        }
        if (g_atomic_int_compare_and_exchange(&redraw_pending, 0, 1)) g_idle_add(repaint, NULL);
    }
}

static void failure(const char *fallback) {
    char *error = office->pClass->getError(office);
    gtk_label_set_text(GTK_LABEL(status), error && *error ? error : fallback);
    if (error) office->pClass->freeError(error);
}

static gboolean draw(GtkWidget *widget, cairo_t *cr, gpointer unused) {
    if (!doc) return FALSE;
    int width = gtk_widget_get_allocated_width(widget), height = gtk_widget_get_allocated_height(widget);
    if (width < 2 || height < 2 || (guint64)width * height > 16000000) return FALSE;
    long dw, dh;
    doc->pClass->getDocumentSize(doc, &dw, &dh);
    if (dw <= 0 || dh <= 0) return FALSE;
    units = MAX(1.0, (double)dw / width);
    double visible = height * units;
    double top = CLAMP(gtk_adjustment_get_value(scroll), 0, MAX(0, dh-visible));
    gtk_adjustment_configure(scroll, top, 0, MAX(dh, visible), units * 44, visible * 0.9, visible);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS) {
        unsigned char *data = cairo_image_surface_get_data(surface);
        doc->pClass->paintTile(doc, data, width, height, 0, (int)top, (int)(width*units), (int)visible);
        if (doc->pClass->getTileMode(doc) == LOK_TILEMODE_RGBA) {
            for (size_t i = 0; i < (size_t)width*height; ++i) {
                unsigned char *pixel = data + 4*i;
                unsigned char r = pixel[0]; pixel[0] = pixel[2]; pixel[2] = r;
            }
        }
        cairo_surface_mark_dirty(surface);
        cairo_set_source_surface(cr, surface, 0, 0);
        cairo_paint(cr);
        if (gtk_widget_has_focus(widget) && g_atomic_int_get(&caret_height) > 0) {
            cairo_set_source_rgb(cr, 0, 0, 0);
            cairo_rectangle(cr, g_atomic_int_get(&caret_x)/units, (g_atomic_int_get(&caret_y)-top)/units,
                            1, g_atomic_int_get(&caret_height)/units);
            cairo_fill(cr);
        }
    }
    cairo_surface_destroy(surface);
    return FALSE;
}

static gboolean discard(void) {
    if (!dirty) return TRUE;
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "文档有未保存的修改。要放弃这些修改吗？");
    gtk_dialog_add_buttons(GTK_DIALOG(dialog), "继续编辑", GTK_RESPONSE_CANCEL,
                           "放弃修改", GTK_RESPONSE_ACCEPT, NULL);
    int answer = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return answer == GTK_RESPONSE_ACCEPT;
}

/* LibreOfficeKit's compact font reply is a flat JSON object:
 * {"FontNames":["Arial", ...], "FontSizes":[8, ...]}. */
static const char *json_array(const char *json, const char *key) {
    const char *start = strstr(json, key);
    if (!start) return NULL;
    start = strchr(start, '[');
    return start ? start + 1 : NULL;
}

static char *json_next_string(const char **cursor) {
    const char *p = *cursor;
    while (*p && *p != '"' && *p != ']') ++p;
    if (*p != '"') { *cursor = p; return NULL; }
    ++p;
    GString *text = g_string_new(NULL);
    while (*p && *p != '"') {
        if (*p == '\\') {
            ++p;
            switch (*p) {
            case '"': g_string_append_c(text, '"'); break;
            case '\\': g_string_append_c(text, '\\'); break;
            case '/': g_string_append_c(text, '/'); break;
            case 'n': g_string_append_c(text, '\n'); break;
            case 'r': g_string_append_c(text, '\r'); break;
            case 't': g_string_append_c(text, '\t'); break;
            case 'u': {
                char hex[5] = {p[1], p[2], p[3], p[4], 0};
                gunichar ch = (gunichar)strtoul(hex, NULL, 16);
                char utf8[8];
                int length = g_unichar_to_utf8(ch, utf8);
                g_string_append_len(text, utf8, length);
                p += 4;
                break;
            }
            default: if (*p) g_string_append_c(text, *p);
            }
            if (*p) ++p;
        } else {
            g_string_append_c(text, *p++);
        }
    }
    if (*p == '"') ++p;
    *cursor = p;
    return g_string_free(text, FALSE);
}

static void load_fonts(void) {
    if (!doc) return;
    ++syncing;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(font_combo));
    char *json = doc->pClass->getCommandValues(doc, ".uno:CharFontName");
    const char *cursor = json ? json_array(json, "\"FontNames\"") : NULL;
    while (cursor) {
        char *name = json_next_string(&cursor);
        if (!name) break;
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(font_combo), name);
        g_free(name);
    }
    free(json);
    --syncing;
}

static void reset_controls(void) {
    syncing++;
    combo_set_text(font_combo, "");
    combo_set_text(size_combo, "12");
    combo_set_text(style_combo, "正文");
    toggle_set(bold_button, FALSE);
    toggle_set(italic_button, FALSE);
    toggle_set(underline_button, FALSE);
    set_align(0, TRUE);
    toggle_set(bullet_button, FALSE);
    GdkRGBA black = {0, 0, 0, 1};
    gtk_color_button_set_rgba(GTK_COLOR_BUTTON(color_button), &black);
    syncing--;
    gtk_widget_set_sensitive(format_bar, doc != NULL);
}

static void open_document(const char *name) {
    char *url = name ? g_filename_to_uri(name, NULL, NULL) : g_strdup("private:factory/swriter");
    LibreOfficeKitDocument *next = url ? office->pClass->documentLoad(office, url) : NULL;
    g_free(url);
    if (!next) { failure("无法打开文档"); return; }
    if (next->pClass->getDocumentType(next) != LOK_DOCTYPE_TEXT) {
        next->pClass->destroy(next);
        gtk_label_set_text(GTK_LABEL(status), "请选择 Writer 文本文档");
        return;
    }
    if (doc) doc->pClass->destroy(doc);
    doc = next;
    g_free(filename); filename = g_strdup(name);
    g_atomic_int_set(&caret_height, 0);
    reset_controls();
    doc->pClass->registerCallback(doc, notification, NULL);
    doc->pClass->initializeForRendering(doc, "{}");
    load_fonts();
    gtk_adjustment_set_value(scroll, 0);
    gtk_label_set_text(GTK_LABEL(status), "");
    set_dirty(FALSE);
    gtk_widget_grab_focus(canvas);
    gtk_widget_queue_draw(canvas);
}

static char *choose(GtkFileChooserAction action, const char *title, const char *initial) {
    GtkWidget *dlg = gtk_file_chooser_dialog_new(title, GTK_WINDOW(window), action, "取消",
        GTK_RESPONSE_CANCEL, action == GTK_FILE_CHOOSER_ACTION_OPEN ? "打开" : "保存", GTK_RESPONSE_ACCEPT, NULL);
    if (action == GTK_FILE_CHOOSER_ACTION_SAVE) {
        gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), initial);
    }
    char *result = gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT ?
        gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg)) : NULL;
    gtk_widget_destroy(dlg);
    return result;
}

static void save(gboolean pdf, gboolean save_as) {
    if (!doc) return;
    char *name = (!pdf && !save_as && filename) ? g_strdup(filename) :
        choose(GTK_FILE_CHOOSER_ACTION_SAVE, pdf ? "导出 PDF" : "保存文档", pdf ? "文档.pdf" : "文档.odt");
    if (!name) return;
    const char *ext = strrchr(name, '.');
    const char *format = pdf ? "pdf" : ext && !g_ascii_strcasecmp(ext, ".docx") ? "docx" :
        ext && !g_ascii_strcasecmp(ext, ".odt") ? "odt" : NULL;
    if (!format) {
        gtk_label_set_text(GTK_LABEL(status), "请另存为 .odt 或 .docx"); g_free(name); return;
    }
    char *url = g_filename_to_uri(name, NULL, NULL);
    if (url && doc->pClass->saveAs(doc, url, format, pdf ? NULL : "TakeOwnership")) {
        if (!pdf) { g_free(filename); filename = g_strdup(name); set_dirty(FALSE); }
        gtk_label_set_text(GTK_LABEL(status), pdf ? "PDF 已导出" : "文档已保存");
    } else failure("写入失败");
    g_free(url); g_free(name);
}

static void action(GtkWidget *button, gpointer operation) {
    const char *name = static_cast<const char *>(operation);
    if (!strcmp(name, "open")) {
        if (!discard()) return;
        char *path = choose(GTK_FILE_CHOOSER_ACTION_OPEN, "打开文档", NULL);
        if (path) { open_document(path); g_free(path); }
    } else if (!strcmp(name, "new")) {
        if (discard()) open_document(NULL);
    } else if (!strcmp(name, "save")) save(FALSE, FALSE);
    else if (!strcmp(name, "save-as")) save(FALSE, TRUE);
    else if (!strcmp(name, "pdf")) save(TRUE, TRUE);
    else if (doc && (!strcmp(name, "copy") || !strcmp(name, "cut"))) {
        char *selection = doc->pClass->getTextSelection(doc, "text/plain;charset=utf-8", NULL);
        if (selection && *selection) {
            gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), selection, -1);
            if (!strcmp(name, "cut")) {
                doc->pClass->postUnoCommand(doc, ".uno:Cut", NULL, false);
                set_dirty(TRUE);
            }
        }
        free(selection);
    } else if (doc && !strcmp(name, "paste")) {
        char *text = gtk_clipboard_wait_for_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
        if (text && doc->pClass->paste(doc, "text/plain;charset=utf-8", text, strlen(text))) set_dirty(TRUE);
        g_free(text);
    } else if (doc) {
        doc->pClass->postUnoCommand(doc, name, NULL, false);
        if (strcmp(name, ".uno:SelectAll")) set_dirty(TRUE);
        gtk_widget_queue_draw(canvas);
    }
    gtk_widget_grab_focus(canvas);
}

static gboolean find_key(GtkWidget *widget, GdkEventKey *event, gpointer unused) {
    if (event->state & GDK_CONTROL_MASK) {
        guint key = gdk_keyval_to_lower(event->keyval);
        if (key == GDK_KEY_f) { show_find(FALSE); return TRUE; }
        if (key == GDK_KEY_h) { show_find(TRUE); return TRUE; }
    }
    if (event->keyval == GDK_KEY_Escape && gtk_widget_get_visible(find_bar)) {
        gtk_widget_hide(find_bar);
        gtk_widget_grab_focus(canvas);
        return TRUE;
    }
    return FALSE;
}

static gboolean focus_find(gpointer data) {
    GtkWidget *entry = GTK_WIDGET(data);
    gtk_widget_grab_focus(entry);
    gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
    return G_SOURCE_REMOVE;
}

static void show_find(gboolean replace) {
    if (!doc) return;
    gtk_widget_show_all(find_bar);
    GtkWidget *entry = replace ? replace_entry : find_entry;
    g_idle_add(focus_find, entry);
}

static char *json_quote(const char *text) {
    GString *out = g_string_new("\"");
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; ++p) {
        switch (*p) {
        case '"': g_string_append(out, "\\\""); break;
        case '\\': g_string_append(out, "\\\\"); break;
        case '\n': g_string_append(out, "\\n"); break;
        case '\r': g_string_append(out, "\\r"); break;
        case '\t': g_string_append(out, "\\t"); break;
        default:
            if (*p < 0x20) g_string_append_printf(out, "\\u%04x", *p);
            else g_string_append_c(out, *p);
        }
    }
    g_string_append_c(out, '"');
    return g_string_free(out, FALSE);
}

/* Mirrors svl/srchitem.hxx: FIND=0, FIND_ALL=1, REPLACE=2, REPLACE_ALL=3. */
static void execute_search(int command, gboolean backward, const char *find, const char *replace) {
    if (!doc) return;
    char *find_json = json_quote(find), *replace_json = json_quote(replace);
    GString *arguments = g_string_new("{\"SearchItem.SearchString\":{\"type\":\"string\",\"value\":");
    g_string_append(arguments, find_json);
    g_string_append(arguments, "},\"SearchItem.Backward\":{\"type\":\"boolean\",\"value\":\"");
    g_string_append(arguments, backward ? "true" : "false");
    g_string_append(arguments, "\"}");
    g_string_append_printf(arguments,
        ",\"SearchItem.Command\":{\"type\":\"short\",\"value\":\"%d\"}", command);
    if (command >= 2) {
        g_string_append(arguments, ",\"SearchItem.ReplaceString\":{\"type\":\"string\",\"value\":");
        g_string_append(arguments, replace_json);
        g_string_append_c(arguments, '}');
    }
    g_string_append_c(arguments, '}');
    doc->pClass->postUnoCommand(doc, ".uno:ExecuteSearch", arguments->str, false);
    if (command >= 2) {
        set_dirty(TRUE);
        gtk_widget_queue_draw(canvas);
    }
    g_string_free(arguments, TRUE);
    g_free(find_json);
    g_free(replace_json);
}

static void find_next(gboolean backward) {
    const char *text = gtk_entry_get_text(GTK_ENTRY(find_entry));
    if (!*text) { gtk_label_set_text(GTK_LABEL(status), "请输入查找内容"); return; }
    execute_search(0, backward, text, NULL);
}

static char *selection_text(void) {
    if (!doc) return NULL;
    char *selection = doc->pClass->getTextSelection(doc, "text/plain;charset=utf-8", NULL);
    return selection;
}

static void replace_one(void) {
    const char *find = gtk_entry_get_text(GTK_ENTRY(find_entry));
    const char *replace = gtk_entry_get_text(GTK_ENTRY(replace_entry));
    if (!*find) { gtk_label_set_text(GTK_LABEL(status), "请输入查找内容"); return; }
    char *selection = selection_text();
    if (selection && !strcmp(selection, find)) {
        execute_search(2, FALSE, find, replace);
        gtk_label_set_text(GTK_LABEL(status), "已替换，并定位到下一处");
    } else {
        execute_search(0, FALSE, find, NULL);
        gtk_label_set_text(GTK_LABEL(status), "已定位，再按「替换」替换当前匹配");
    }
    free(selection);
}

static void replace_all(void) {
    const char *find = gtk_entry_get_text(GTK_ENTRY(find_entry));
    const char *replace = gtk_entry_get_text(GTK_ENTRY(replace_entry));
    if (!*find) { gtk_label_set_text(GTK_LABEL(status), "请输入查找内容"); return; }
    execute_search(3, FALSE, find, replace);
    gtk_label_set_text(GTK_LABEL(status), "已全部替换");
}

static void find_activate(GtkEntry *entry, gpointer unused) { find_next(FALSE); }
static void find_previous(GtkButton *button, gpointer unused) { find_next(TRUE); }
static void find_forward(GtkButton *button, gpointer unused) { find_next(FALSE); }
static void close_find(GtkButton *button, gpointer unused) {
    gtk_widget_hide(find_bar);
    gtk_widget_grab_focus(canvas);
}
static void replace_activate(GtkEntry *entry, gpointer unused) { replace_one(); }
static void replace_clicked(GtkButton *button, gpointer unused) { replace_one(); }
static void replace_all_clicked(GtkButton *button, gpointer unused) { replace_all(); }

static void font_entered(GtkWidget *entry, gpointer unused) {
    combo_typing = FALSE;
    if (!syncing) apply_font(gtk_entry_get_text(GTK_ENTRY(entry)));
    gtk_widget_grab_focus(canvas);
}

static void font_changed(GtkComboBox *combo, gpointer unused) {
    if (syncing || combo_typing) return;
    apply_font(combo_text(GTK_WIDGET(combo)));
    gtk_widget_grab_focus(canvas);
}

static void size_entered(GtkWidget *entry, gpointer unused) {
    combo_typing = FALSE;
    if (!syncing) apply_size(gtk_entry_get_text(GTK_ENTRY(entry)));
    gtk_widget_grab_focus(canvas);
}

static void size_changed(GtkComboBox *combo, gpointer unused) {
    if (syncing || combo_typing) return;
    apply_size(combo_text(GTK_WIDGET(combo)));
    gtk_widget_grab_focus(canvas);
}

static void style_entered(GtkWidget *entry, gpointer unused) {
    combo_typing = FALSE;
    if (!syncing) apply_style(style_value(gtk_entry_get_text(GTK_ENTRY(entry))));
    gtk_widget_grab_focus(canvas);
}

static void style_changed(GtkComboBox *combo, gpointer unused) {
    if (syncing || combo_typing) return;
    apply_style(style_value(combo_text(GTK_WIDGET(combo))));
    gtk_widget_grab_focus(canvas);
}

static void toggle_format(GtkToggleButton *button, gpointer operation) {
    if (syncing) return;
    apply_format(static_cast<const char *>(operation), NULL);
    gtk_widget_grab_focus(canvas);
}

static void align_format(GtkToggleButton *button, gpointer operation) {
    if (syncing || !gtk_toggle_button_get_active(button)) return;
    syncing++;
    for (int i = 0; i < 4; ++i)
        if (GTK_WIDGET(button) != align_buttons[i]) toggle_set(align_buttons[i], FALSE);
    syncing--;
    apply_format(static_cast<const char *>(operation), NULL);
    gtk_widget_grab_focus(canvas);
}

static void color_changed(GtkColorButton *button, gpointer unused) {
    if (syncing || !doc) return;
    GdkRGBA rgba;
    gtk_color_button_get_rgba(button, &rgba);
    int red = (int)(rgba.red * 255 + .5), green = (int)(rgba.green * 255 + .5),
        blue = (int)(rgba.blue * 255 + .5);
    char *arguments = g_strdup_printf(
        "{\"Color\":{\"type\":\"long\",\"value\":\"%d\"}}", (red << 16) | (green << 8) | blue);
    apply_format(".uno:Color", arguments);
    g_free(arguments);
    gtk_widget_grab_focus(canvas);
}

static gboolean shortcut(GdkEventKey *event) {
    if (!(event->state & GDK_CONTROL_MASK)) return FALSE;
    const char *op = NULL;
    switch (gdk_keyval_to_lower(event->keyval)) {
    case GDK_KEY_s: op = "save"; break;
    case GDK_KEY_o: op = "open"; break;
    case GDK_KEY_n: op = "new"; break;
    case GDK_KEY_c: op = "copy"; break;
    case GDK_KEY_x: op = "cut"; break;
    case GDK_KEY_v: op = "paste"; break;
    case GDK_KEY_a: op = ".uno:SelectAll"; break;
    case GDK_KEY_z: op = ".uno:Undo"; break;
    case GDK_KEY_y: op = ".uno:Redo"; break;
    case GDK_KEY_b: op = ".uno:Bold"; break;
    case GDK_KEY_i: op = ".uno:Italic"; break;
    case GDK_KEY_u: op = ".uno:Underline"; break;
    case GDK_KEY_f: show_find(FALSE); return TRUE;
    case GDK_KEY_h: show_find(TRUE); return TRUE;
    case GDK_KEY_1: apply_style("Heading 1"); return TRUE;
    case GDK_KEY_2: apply_style("Heading 2"); return TRUE;
    case GDK_KEY_3: apply_style("Heading 3"); return TRUE;
    case GDK_KEY_0: apply_style("Text body"); return TRUE;
    }
    if (op) { action(NULL, (gpointer)op); return TRUE; }
    return FALSE;
}

static gboolean window_key(GtkWidget *widget, GdkEventKey *event, gpointer unused) {
    if (shortcut(event)) return TRUE;
    if (event->keyval == GDK_KEY_Escape && gtk_widget_get_visible(find_bar)) {
        gtk_widget_hide(find_bar);
        gtk_widget_grab_focus(canvas);
        return TRUE;
    }
    return FALSE;
}

static gboolean key(GtkWidget *widget, GdkEventKey *e, gpointer unused) {
    if (shortcut(e)) return TRUE;
    if (!doc) return FALSE;
    int code = 0;
    switch (e->keyval) {
    case GDK_KEY_Down: code = 1024; break;
    case GDK_KEY_Up: code = 1025; break;
    case GDK_KEY_Left: code = 1026; break;
    case GDK_KEY_Right: code = 1027; break;
    case GDK_KEY_Home: code = 1028; break;
    case GDK_KEY_End: code = 1029; break;
    case GDK_KEY_Return: code = 1280; break;
    case GDK_KEY_Tab: code = 1282; break;
    case GDK_KEY_BackSpace: code = 1283; break;
    case GDK_KEY_Delete: code = 1286; break;
    }
    gunichar ch = code ? 0 : gdk_keyval_to_unicode(e->keyval);
    if (!code && (!ch || (e->state & (GDK_CONTROL_MASK|GDK_MOD1_MASK)))) return FALSE;
    if (ch || code >= 1280) set_dirty(TRUE);
    if (e->state & GDK_SHIFT_MASK) code |= 4096;
    doc->pClass->postKeyEvent(doc, LOK_KEYEVENT_KEYINPUT, ch, code);
    doc->pClass->postKeyEvent(doc, LOK_KEYEVENT_KEYUP, ch, code);
    return TRUE;
}

static gboolean mouse(GtkWidget *widget, GdkEventButton *e, gpointer unused) {
    if (!doc || e->button != 1) return FALSE;
    gtk_widget_grab_focus(canvas);
    doc->pClass->postMouseEvent(doc, e->type == GDK_BUTTON_RELEASE ? LOK_MOUSEEVENT_MOUSEBUTTONUP :
        LOK_MOUSEEVENT_MOUSEBUTTONDOWN, (int)(e->x*units), (int)(e->y*units+gtk_adjustment_get_value(scroll)),
        e->type == GDK_2BUTTON_PRESS ? 2 : 1, 1, 0);
    return TRUE;
}

static gboolean drag(GtkWidget *widget, GdkEventMotion *e, gpointer unused) {
    if (!doc || !(e->state & GDK_BUTTON1_MASK)) return FALSE;
    doc->pClass->postMouseEvent(doc, LOK_MOUSEEVENT_MOUSEMOVE, (int)(e->x*units),
                                (int)(e->y*units + gtk_adjustment_get_value(scroll)), 1, 1, 0);
    return TRUE;
}

static gboolean wheel(GtkWidget *w, GdkEventScroll *e, gpointer data) {
    double dx, dy;
    if (!gdk_event_get_scroll_deltas((GdkEvent *)e, &dx, &dy))
        dy = e->direction == GDK_SCROLL_UP ? -1 : 1;
    gtk_adjustment_set_value(scroll, gtk_adjustment_get_value(scroll) + dy*44*units);
    return TRUE;
}

static gboolean close_request(GtkWidget *w, GdkEvent *event, gpointer data) { return !discard(); }

extern "C" void writer_environment(void) {
    g_setenv("HOME", "/home/lixin", FALSE);
    g_setenv("TMPDIR", "/tmp", FALSE);
    g_setenv("URE_BOOTSTRAP", "vnd.sun.star.pathname:/lib/writer/program/fundamentalrc", FALSE);
    g_setenv("BRAND_BASE_DIR", "file:///lib/writer", FALSE);
    g_setenv("FONTCONFIG_FILE", "/lib/gtk3/fonts.conf", TRUE);
    g_setenv("SAL_USE_VCLPLUGIN", "svp", TRUE);
    g_setenv("SAL_LOK_OPTIONS", "compact_fonts", TRUE);
}

static GtkWidget *toolbar_button(const char *label, const char *operation, const char *tip) {
    GtkWidget *button = gtk_button_new_with_label(label);
    gtk_widget_set_tooltip_text(button, tip);
    g_signal_connect(button, "clicked", G_CALLBACK(action), (gpointer)operation);
    return button;
}

static GtkWidget *combo_box(const char *tip, int width, gboolean entry) {
    GtkWidget *combo = entry ? gtk_combo_box_text_new_with_entry() : gtk_combo_box_text_new();
    gtk_widget_set_tooltip_text(combo, tip);
    gtk_widget_set_size_request(combo, width, -1);
    return combo;
}

extern "C" int writer_window(int argc, char **argv) {
    g_setenv("NO_AT_BRIDGE", "1", TRUE);
    g_setenv("GDK_BACKEND", "broadway", TRUE);
    g_setenv("GSETTINGS_BACKEND", "memory", TRUE);
    if (!gtk_init_check(&argc, &argv)) return 1;
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "LibreOffice Writer");
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 520);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2), *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_container_add(GTK_CONTAINER(window), box);
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);
    const char *actions[][2] = {{"新建","new"},{"打开","open"},{"保存","save"},{"另存为","save-as"},
        {"导出 PDF","pdf"},{"撤销",".uno:Undo"},{"重做",".uno:Redo"}};
    for (unsigned i = 0; i < G_N_ELEMENTS(actions); ++i)
        gtk_box_pack_start(GTK_BOX(bar), toolbar_button(actions[i][0], actions[i][1], actions[i][0]),
                           FALSE, FALSE, 0);

    format_bar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(box), format_bar, FALSE, FALSE, 0);
    GtkWidget *format_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    GtkWidget *tools_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_box_pack_start(GTK_BOX(format_bar), format_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(format_bar), tools_row, FALSE, FALSE, 0);
    style_combo = combo_box("段落样式：正文、标题 1、标题 2、标题 3", 96, TRUE);
    for (unsigned i = 0; i < G_N_ELEMENTS(styles); ++i)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(style_combo), styles[i].label);
    gtk_box_pack_start(GTK_BOX(format_row), style_combo, FALSE, FALSE, 0);
    font_combo = combo_box("字体", 140, TRUE);
    gtk_box_pack_start(GTK_BOX(format_row), font_combo, FALSE, FALSE, 0);
    size_combo = combo_box("字号（磅）", 56, TRUE);
    const char *sizes[] = {"8","9","10","10.5","11","12","14","16","18","20","22","24","26","28","36","48","72"};
    for (unsigned i = 0; i < G_N_ELEMENTS(sizes); ++i)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(size_combo), sizes[i]);
    gtk_box_pack_start(GTK_BOX(format_row), size_combo, FALSE, FALSE, 0);
    bold_button = gtk_toggle_button_new_with_label("B");
    italic_button = gtk_toggle_button_new_with_label("I");
    underline_button = gtk_toggle_button_new_with_label("U");
    gtk_widget_set_size_request(bold_button, 30, -1);
    gtk_widget_set_size_request(italic_button, 30, -1);
    gtk_widget_set_size_request(underline_button, 30, -1);
    gtk_widget_set_tooltip_text(bold_button, "粗体");
    gtk_widget_set_tooltip_text(italic_button, "斜体");
    gtk_widget_set_tooltip_text(underline_button, "下划线");
    g_signal_connect(bold_button, "toggled", G_CALLBACK(toggle_format), (gpointer)".uno:Bold");
    g_signal_connect(italic_button, "toggled", G_CALLBACK(toggle_format), (gpointer)".uno:Italic");
    g_signal_connect(underline_button, "toggled", G_CALLBACK(toggle_format), (gpointer)".uno:Underline");
    gtk_box_pack_start(GTK_BOX(tools_row), bold_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tools_row), italic_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tools_row), underline_button, FALSE, FALSE, 0);

    const char *align_labels[] = {"左", "中", "右", "齐"};
    const char *align_tips[] = {"左对齐", "居中", "右对齐", "两端对齐"};
    const char *align_ops[] = {".uno:LeftPara", ".uno:CenterPara", ".uno:RightPara", ".uno:JustifyPara"};
    for (unsigned i = 0; i < G_N_ELEMENTS(align_buttons); ++i) {
        align_buttons[i] = gtk_toggle_button_new_with_label(align_labels[i]);
        gtk_widget_set_size_request(align_buttons[i], 34, -1);
        gtk_widget_set_tooltip_text(align_buttons[i], align_tips[i]);
        g_signal_connect(align_buttons[i], "toggled", G_CALLBACK(align_format), (gpointer)align_ops[i]);
        gtk_box_pack_start(GTK_BOX(tools_row), align_buttons[i], FALSE, FALSE, 0);
    }
    bullet_button = gtk_toggle_button_new_with_label("•");
    gtk_widget_set_size_request(bullet_button, 32, -1);
    gtk_widget_set_tooltip_text(bullet_button, "项目符号");
    g_signal_connect(bullet_button, "toggled", G_CALLBACK(toggle_format), (gpointer)".uno:DefaultBullet");
    gtk_box_pack_start(GTK_BOX(tools_row), bullet_button, FALSE, FALSE, 0);
    GdkRGBA black = {0, 0, 0, 1};
    color_button = gtk_color_button_new_with_rgba(&black);
    gtk_widget_set_size_request(color_button, 44, -1);
    gtk_widget_set_tooltip_text(color_button, "文字颜色");
    gtk_color_button_set_title(GTK_COLOR_BUTTON(color_button), "文字颜色");
    g_signal_connect(color_button, "color-set", G_CALLBACK(color_changed), NULL);
    gtk_box_pack_start(GTK_BOX(tools_row), color_button, FALSE, FALSE, 0);

    find_bar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *find_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_box_pack_start(GTK_BOX(find_bar), find_row, FALSE, FALSE, 0);
    GtkWidget *find_label = gtk_label_new("查找");
    gtk_box_pack_start(GTK_BOX(find_row), find_label, FALSE, FALSE, 0);
    find_entry = gtk_entry_new();
    gtk_widget_set_hexpand(find_entry, TRUE);
    gtk_box_pack_start(GTK_BOX(find_row), find_entry, TRUE, TRUE, 0);
    GtkWidget *previous = gtk_button_new_with_label("上一个");
    GtkWidget *next = gtk_button_new_with_label("下一个");
    GtkWidget *close = gtk_button_new_with_label("关闭");
    gtk_box_pack_start(GTK_BOX(find_row), previous, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(find_row), next, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(find_row), close, FALSE, FALSE, 0);
    replace_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_box_pack_start(GTK_BOX(find_bar), replace_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(replace_row), gtk_label_new("替换"), FALSE, FALSE, 0);
    replace_entry = gtk_entry_new();
    gtk_widget_set_hexpand(replace_entry, TRUE);
    gtk_box_pack_start(GTK_BOX(replace_row), replace_entry, TRUE, TRUE, 0);
    GtkWidget *replace = gtk_button_new_with_label("替换");
    GtkWidget *all = gtk_button_new_with_label("全部替换");
    gtk_box_pack_start(GTK_BOX(replace_row), replace, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(replace_row), all, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), find_bar, FALSE, FALSE, 0);
    g_signal_connect(find_entry, "activate", G_CALLBACK(find_activate), NULL);
    g_signal_connect(previous, "clicked", G_CALLBACK(find_previous), NULL);
    g_signal_connect(next, "clicked", G_CALLBACK(find_forward), NULL);
    g_signal_connect(close, "clicked", G_CALLBACK(close_find), NULL);
    g_signal_connect(replace_entry, "activate", G_CALLBACK(replace_activate), NULL);
    g_signal_connect(replace, "clicked", G_CALLBACK(replace_clicked), NULL);
    g_signal_connect(all, "clicked", G_CALLBACK(replace_all_clicked), NULL);
    g_signal_connect(find_entry, "key-press-event", G_CALLBACK(find_key), NULL);
    g_signal_connect(replace_entry, "key-press-event", G_CALLBACK(find_key), NULL);
    g_signal_connect(style_combo, "changed", G_CALLBACK(style_changed), NULL);
    g_signal_connect(font_combo, "changed", G_CALLBACK(font_changed), NULL);
    g_signal_connect(size_combo, "changed", G_CALLBACK(size_changed), NULL);
    g_signal_connect(style_combo, "key-press-event", G_CALLBACK(combo_key_press), NULL);
    g_signal_connect(font_combo, "key-press-event", G_CALLBACK(combo_key_press), NULL);
    g_signal_connect(size_combo, "key-press-event", G_CALLBACK(combo_key_press), NULL);
    GtkWidget *style_entry = combo_entry(style_combo), *font_entry = combo_entry(font_combo),
              *size_entry = combo_entry(size_combo);
    if (style_entry) {
        g_signal_connect(style_entry, "activate", G_CALLBACK(style_entered), NULL);
        g_signal_connect(style_entry, "key-press-event", G_CALLBACK(find_key), NULL);
        g_signal_connect(style_entry, "key-press-event", G_CALLBACK(combo_key_press), NULL);
    }
    if (font_entry) {
        g_signal_connect(font_entry, "activate", G_CALLBACK(font_entered), NULL);
        g_signal_connect(font_entry, "key-press-event", G_CALLBACK(find_key), NULL);
        g_signal_connect(font_entry, "key-press-event", G_CALLBACK(combo_key_press), NULL);
    }
    if (size_entry) {
        g_signal_connect(size_entry, "activate", G_CALLBACK(size_entered), NULL);
        g_signal_connect(size_entry, "key-press-event", G_CALLBACK(find_key), NULL);
        g_signal_connect(size_entry, "key-press-event", G_CALLBACK(combo_key_press), NULL);
    }

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(box), body, TRUE, TRUE, 0);
    canvas = gtk_drawing_area_new();
    gtk_widget_set_can_focus(canvas, TRUE);
    gtk_widget_add_events(canvas, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_BUTTON_MOTION_MASK | GDK_SCROLL_MASK);
    gtk_box_pack_start(GTK_BOX(body), canvas, TRUE, TRUE, 0);
    scroll = gtk_adjustment_new(0, 0, 1, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(body), gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL, scroll), FALSE, FALSE, 0);
    status = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(box), status, FALSE, FALSE, 0);
    g_signal_connect_swapped(scroll, "value-changed", G_CALLBACK(gtk_widget_queue_draw), canvas);
    g_signal_connect(canvas, "draw", G_CALLBACK(draw), NULL);
    g_signal_connect(canvas, "key-press-event", G_CALLBACK(key), NULL);
    g_signal_connect(canvas, "button-press-event", G_CALLBACK(mouse), NULL);
    g_signal_connect(canvas, "button-release-event", G_CALLBACK(mouse), NULL);
    g_signal_connect(canvas, "motion-notify-event", G_CALLBACK(drag), NULL);
    g_signal_connect(canvas, "scroll-event", G_CALLBACK(wheel), NULL);
    g_signal_connect(window, "key-press-event", G_CALLBACK(window_key), NULL);
    g_signal_connect(window, "delete-event", G_CALLBACK(close_request), NULL);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    office = libreofficekit_hook_2("/lib/writer/program", NULL);
    if (!office) { g_printerr("LibreOffice 初始化失败\n"); return 1; }
    reset_controls();
    gtk_widget_show_all(window);
    gtk_widget_hide(find_bar);
    char *file = argc > 1 ? g_canonicalize_filename(argv[1], NULL) : NULL;
    open_document(file);
    g_free(file);
    gtk_main();
    canvas = NULL;
    if (doc) doc->pClass->destroy(doc);
    office->pClass->destroy(office);
    g_free(filename);
    return 0;
}
