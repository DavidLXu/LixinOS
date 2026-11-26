#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib/gstdio.h>
#include "../tcc/lixin.h"
#include "pdf.h"

#define HISTORY_MAX 20
#define THUMB_WIDTH 96
#define THUMB_HEIGHT 128
#define STATE_LIMIT 32768

typedef struct {
    char path[513];
    int page;
    double scroll;
} Reading;

static GtkWidget *window, *path, *image, *scrollbox, *status, *pagebox, *back, *forward;
static GtkWidget *sidebar, *thumbbox, *thumbtoggle;
static GtkWidget **thumbbuttons, **thumbimages;
static GdkPixbuf **thumbpix;
static int thumb_count, thumb_next;
static guint thumb_idle;
static gboolean thumbs_visible = TRUE, thumb_sync;
static PdfFile *doc;
static int page, last_width;
static double zoom;
static gboolean fit = TRUE;
static guint redraw;

static char current_path[513];
static Reading history[HISTORY_MAX];
static int history_count;
static char state_file[600];
static guint save_timer;
static gboolean scroll_restoring, pending_scroll_valid;
static double pending_scroll;

static void render(void);
static gboolean render_idle(gpointer unused);
static void sync_thumb(int n);
static void start_thumbs(void);

static void release_pixels(guchar *pixels, gpointer unused) { free(pixels); }
static GdkPixbuf *pdf_render(PdfFile *file, int n, double scale) {
    int width, height;
    unsigned char *rgb = pdf_render_rgb(file, n, scale, &width, &height);
    if (!rgb) return NULL;
    return gdk_pixbuf_new_from_data(rgb, GDK_COLORSPACE_RGB, FALSE, 8, width, height,
                                    width * 3, release_pixels, NULL);
}

static const char *home_state_file(void) {
    static char candidate[600];
    const char *home = getenv("HOME");
    if (!home || home[0] != '/' || strlen(home) >= 540) return NULL;
    g_snprintf(candidate, sizeof candidate, "%s/.pdfview-state", home);
    return candidate;
}

static const char *choose_state_file(void) {
    if (state_file[0]) return state_file;
    const char *primary = "/home/lixin/.pdfview-state";
    const char *fallback = home_state_file();
    const char *chosen = primary;
    FILE *probe = fopen(primary, "r");
    if (!probe && fallback && strcmp(fallback, primary)) {
        probe = fopen(fallback, "r");
        if (probe) chosen = fallback;
    }
    if (probe) fclose(probe);
    g_strlcpy(state_file, chosen, sizeof state_file);
    return state_file;
}

static void make_parent(const char *file) {
    char directory[600];
    g_strlcpy(directory, file, sizeof directory);
    char *slash = strrchr(directory, '/');
    if (!slash) return;
    *slash = 0;
    if (directory[0]) g_mkdir_with_parents(directory, 0700);
}

static void load_state(void) {
    FILE *stream = fopen(choose_state_file(), "r");
    if (!stream) return;
    char line[800];
    while (history_count < HISTORY_MAX && fgets(line, sizeof line, stream)) {
        int n;
        double fraction;
        char name[513];
        if (sscanf(line, "%d\t%lf\t%512[^\n]", &n, &fraction, name) != 3 || !name[0]) continue;
        if (n < 1) continue;
        Reading *entry = &history[history_count++];
        g_strlcpy(entry->path, name, sizeof entry->path);
        entry->page = n;
        entry->scroll = CLAMP(fraction, 0, 1);
    }
    fclose(stream);
}

static void save_state(void) {
    char data[STATE_LIMIT];
    size_t used = 0;
    for (int i = 0; i < history_count; ++i) {
        if (used + 540 > sizeof data) break;
        int n = g_snprintf(data + used, sizeof data - used, "%d\t%.6f\t%s\n",
                           history[i].page, history[i].scroll, history[i].path);
        if (n < 0 || (size_t)n >= sizeof data - used) break;
        used += (size_t)n;
    }
    if (!used) return;
    const char *file = choose_state_file();
    make_parent(file);
    if (lx_save_file(file, data, (unsigned)used) >= 0) return;
    const char *alternatives[] = {"/home/lixin/.pdfview-state", home_state_file(), ".pdfview-state"};
    for (unsigned i = 0; i < G_N_ELEMENTS(alternatives); ++i) {
        const char *candidate = alternatives[i];
        if (!candidate || !strcmp(candidate, file)) continue;
        make_parent(candidate);
        if (lx_save_file(candidate, data, (unsigned)used) >= 0) {
            g_strlcpy(state_file, candidate, sizeof state_file);
            return;
        }
    }
}

static gboolean save_timer_done(gpointer unused) {
    save_timer = 0;
    save_state();
    return G_SOURCE_REMOVE;
}
static void queue_save(void) {
    if (save_timer) g_source_remove(save_timer);
    save_timer = g_timeout_add(500, save_timer_done, NULL);
}

static double scroll_fraction(void) {
    GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrollbox));
    double span = gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment);
    if (span <= 0.5) return 0;
    return CLAMP(gtk_adjustment_get_value(adjustment) / span, 0, 1);
}

static Reading *history_find(const char *name) {
    for (int i = 0; i < history_count; ++i)
        if (!strcmp(history[i].path, name)) return &history[i];
    return NULL;
}

static Reading *history_use(const char *name) {
    int index = -1;
    for (int i = 0; i < history_count; ++i)
        if (!strcmp(history[i].path, name)) { index = i; break; }
    if (index < 0) {
        if (history_count == HISTORY_MAX)
            memmove(&history[1], &history[0], (HISTORY_MAX - 1) * sizeof *history);
        else {
            memmove(&history[1], &history[0], (size_t)history_count * sizeof *history);
            ++history_count;
        }
        index = 0;
        memset(&history[index], 0, sizeof history[index]);
        g_strlcpy(history[index].path, name, sizeof history[index].path);
        history[index].page = 1;
    } else if (index > 0) {
        Reading recent = history[index];
        memmove(&history[1], &history[0], (size_t)index * sizeof *history);
        history[0] = recent;
        index = 0;
    }
    return &history[index];
}

static void update_position(void) {
    if (!doc || !current_path[0]) return;
    Reading *entry = history_use(current_path);
    entry->page = page;
    entry->scroll = scroll_fraction();
}

static gboolean clear_pending_scroll(gpointer unused) {
    pending_scroll_valid = FALSE;
    return G_SOURCE_REMOVE;
}

static void adjustment_changed(GtkAdjustment *adjustment, gpointer unused) {
    if (!pending_scroll_valid) return;
    double span = gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment);
    if (span <= 0.5) return;
    scroll_restoring = TRUE;
    gtk_adjustment_set_value(adjustment, pending_scroll * span);
    scroll_restoring = FALSE;
    pending_scroll_valid = FALSE;
    update_position();
    queue_save();
}

static void scroll_value_changed(GtkAdjustment *adjustment, gpointer unused) {
    if (!doc || scroll_restoring || pending_scroll_valid) return;
    update_position();
    queue_save();
}

static void ensure_thumb(int n) {
    if (!doc || n < 1 || n > thumb_count || thumbpix[n]) return;
    double width = pdf_width(doc, n);
    if (!(width > 1)) return;
    double scale = THUMB_WIDTH / width;
    if (scale > 2) scale = 2;
    GdkPixbuf *pix = pdf_render(doc, n, scale);
    if (!pix) return;
    thumbpix[n] = pix;
    if (thumbimages[n]) gtk_image_set_from_pixbuf(GTK_IMAGE(thumbimages[n]), pix);
}

static gboolean thumb_idle_done(gpointer unused) {
    if (!doc || !thumbs_visible) { thumb_idle = 0; return G_SOURCE_REMOVE; }
    while (thumb_next <= thumb_count && thumbpix[thumb_next]) ++thumb_next;
    if (thumb_next > thumb_count) { thumb_idle = 0; return G_SOURCE_REMOVE; }
    ensure_thumb(thumb_next);
    ++thumb_next;
    return G_SOURCE_CONTINUE;
}
static void start_thumbs(void) {
    if (thumb_idle || !doc || !thumbs_visible) return;
    thumb_idle = g_idle_add_full(G_PRIORITY_LOW, thumb_idle_done, NULL, NULL);
}
static void stop_thumbs(void) {
    if (thumb_idle) g_source_remove(thumb_idle);
    thumb_idle = 0;
}

static gboolean reveal_thumb(gpointer data) {
    int n = GPOINTER_TO_INT(data);
    if (!thumbs_visible || n < 1 || n > thumb_count || !thumbbuttons[n]) return G_SOURCE_REMOVE;
    GtkAllocation area;
    gtk_widget_get_allocation(thumbbuttons[n], &area);
    GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sidebar));
    double span = gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment);
    if (span <= 0.5) return G_SOURCE_REMOVE;
    double top = gtk_adjustment_get_value(adjustment);
    double height = gtk_adjustment_get_page_size(adjustment);
    if (area.y < top || area.y + area.height > top + height) {
        double value = area.y + area.height / 2 - height / 2;
        gtk_adjustment_set_value(adjustment, CLAMP(value, 0, span));
    }
    return G_SOURCE_REMOVE;
}

static void thumb_clicked(GtkToggleButton *button, gpointer data) {
    if (thumb_sync || !gtk_toggle_button_get_active(button) || !doc) return;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(pagebox), GPOINTER_TO_INT(data));
}

static void sync_thumb(int n) {
    if (!thumbbuttons || n < 1 || n > thumb_count || !thumbbuttons[n]) return;
    thumb_sync = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(thumbbuttons[n]), TRUE);
    thumb_sync = FALSE;
    ensure_thumb(n);
    if (thumbs_visible) g_idle_add(reveal_thumb, GINT_TO_POINTER(n));
}

static void clear_thumbs(void) {
    stop_thumbs();
    if (thumbbox) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(thumbbox));
        for (GList *item = children; item; item = item->next) gtk_widget_destroy(GTK_WIDGET(item->data));
        g_list_free(children);
    }
    if (thumbpix) {
        for (int n = 1; n <= thumb_count; ++n)
            if (thumbpix[n]) g_object_unref(thumbpix[n]);
        g_free(thumbpix);
    }
    g_free(thumbbuttons);
    g_free(thumbimages);
    thumbpix = NULL;
    thumbbuttons = thumbimages = NULL;
    thumb_count = thumb_next = 0;
}

static void build_thumbs(void) {
    clear_thumbs();
    if (!doc) return;
    thumb_count = pdf_pages(doc);
    thumbbuttons = g_new0(GtkWidget *, (gsize)thumb_count + 1);
    thumbimages = g_new0(GtkWidget *, (gsize)thumb_count + 1);
    thumbpix = g_new0(GdkPixbuf *, (gsize)thumb_count + 1);
    GtkWidget *group = NULL;
    for (int n = 1; n <= thumb_count; ++n) {
        GtkWidget *button = gtk_radio_button_new(NULL);
        gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(button), FALSE);
        gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *picture = gtk_image_new();
        gtk_widget_set_size_request(picture, THUMB_WIDTH, THUMB_HEIGHT);
        gtk_widget_set_halign(picture, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(picture, GTK_ALIGN_CENTER);
        char label[32];
        g_snprintf(label, sizeof label, "%d", n);
        GtkWidget *number = gtk_label_new(label);
        gtk_box_pack_start(GTK_BOX(box), picture, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), number, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(button), box);
        if (group) gtk_radio_button_join_group(GTK_RADIO_BUTTON(button), GTK_RADIO_BUTTON(group));
        else group = button;
        char tip[64];
        g_snprintf(tip, sizeof tip, "第 %d 页", n);
        gtk_widget_set_tooltip_text(button, tip);
        g_signal_connect(button, "toggled", G_CALLBACK(thumb_clicked), GINT_TO_POINTER(n));
        gtk_box_pack_start(GTK_BOX(thumbbox), button, FALSE, FALSE, 0);
        thumbbuttons[n] = button;
        thumbimages[n] = picture;
    }
    gtk_widget_show_all(thumbbox);
    thumb_next = 1;
    start_thumbs();
}

static void toggle_sidebar(GtkToggleButton *button, gpointer unused) {
    thumbs_visible = gtk_toggle_button_get_active(button);
    gtk_widget_set_visible(sidebar, thumbs_visible);
    if (thumbs_visible) {
        start_thumbs();
        sync_thumb(page);
    } else stop_thumbs();
    last_width = 0;
    if (!redraw) redraw = g_idle_add(render_idle, NULL);
}

static void render(void) {
    if (!doc) return;
    if (fit) zoom = (MAX(100, gtk_widget_get_allocated_width(scrollbox)) - 24) / pdf_width(doc, page);
    GdkPixbuf *pix = pdf_render(doc, page, zoom);
    gtk_image_set_from_pixbuf(GTK_IMAGE(image), pix);
    char info[80];
    if (pix) {
        g_object_unref(pix);
        g_snprintf(info, sizeof info, "/ %d 页    %.0f%%", pdf_pages(doc), zoom * 100);
    } else g_snprintf(info, sizeof info, "页面过大，请缩小");
    gtk_label_set_text(GTK_LABEL(status), info);
    gtk_widget_set_sensitive(back, page > 1);
    gtk_widget_set_sensitive(forward, page < pdf_pages(doc));
    sync_thumb(page);
}

static gboolean render_idle(gpointer unused) { redraw = 0; render(); return G_SOURCE_REMOVE; }
static void resize(GtkWidget *widget, GtkAllocation *a, gpointer unused) {
    if (fit && a->width != last_width) {
        last_width = a->width;
        if (!redraw) redraw = g_idle_add(render_idle, NULL);
    }
}

static gboolean valid_path(const char *name) {
    if (!name[0]) { gtk_label_set_text(GTK_LABEL(status), "请输入 PDF 路径"); return FALSE; }
    if (strlen(name) >= sizeof current_path) {
        gtk_label_set_text(GTK_LABEL(status), "路径过长");
        return FALSE;
    }
    return TRUE;
}

static void open_path(void) {
    const char *name = gtk_entry_get_text(GTK_ENTRY(path));
    if (!valid_path(name)) return;
    const char *message = NULL;
    PdfFile *next = pdf_open(name, &message);
    if (!next) { gtk_label_set_text(GTK_LABEL(status), message); return; }
    Reading *reading = history_use(name);
    pdf_close(doc);
    doc = next;
    g_strlcpy(current_path, name, sizeof current_path);
    page = CLAMP(reading->page, 1, pdf_pages(doc));
    double saved_scroll = CLAMP(reading->scroll, 0, 1);
    pending_scroll_valid = FALSE;
    fit = TRUE;
    scroll_restoring = TRUE;
    gtk_spin_button_set_range(GTK_SPIN_BUTTON(pagebox), 1, pdf_pages(doc));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(pagebox), page);
    gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrollbox)), 0);
    scroll_restoring = FALSE;
    build_thumbs();
    render();
    scroll_restoring = TRUE;
    pending_scroll = saved_scroll;
    pending_scroll_valid = saved_scroll > 0.0001;
    scroll_restoring = FALSE;
    if (pending_scroll_valid) {
        adjustment_changed(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrollbox)), NULL);
        if (pending_scroll_valid) g_timeout_add(600, clear_pending_scroll, NULL);
    }
    Reading *entry = history_find(current_path);
    if (entry) entry->page = page;
    queue_save();
    gtk_widget_grab_focus(scrollbox);
}

static void choose(void) {
    GtkWidget *dlg = gtk_file_chooser_dialog_new("打开 PDF", GTK_WINDOW(window),
        GTK_FILE_CHOOSER_ACTION_OPEN, "取消", GTK_RESPONSE_CANCEL, "打开", GTK_RESPONSE_ACCEPT, NULL);
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "PDF"); gtk_file_filter_add_pattern(filter, "*.[pP][dD][fF]");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), filter);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (filename) { gtk_entry_set_text(GTK_ENTRY(path), filename); g_free(filename); open_path(); }
    }
    gtk_widget_destroy(dlg);
}

static void selected(GtkSpinButton *spin, gpointer unused) {
    int n = gtk_spin_button_get_value_as_int(spin);
    if (!doc || n == page) return;
    page = n;
    pending_scroll_valid = FALSE;
    scroll_restoring = TRUE;
    gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrollbox)), 0);
    scroll_restoring = FALSE;
    render();
    update_position();
    queue_save();
}

static void move(int direction) { if (doc) gtk_spin_button_set_value(GTK_SPIN_BUTTON(pagebox), page + direction); }
static void scale(int direction) {
    if (!doc) return;
    fit = direction == 0;
    if (!fit) zoom = CLAMP(zoom * (direction > 0 ? 1.25 : 0.8), 0.125, 6);
    pending_scroll_valid = FALSE;
    render();
}
static void clicked(GtkButton *button, gpointer action) {
    switch (GPOINTER_TO_INT(action)) {
    case 0: choose(); break;
    case 1: move(-1); break;
    case 2: move(1); break;
    case 3: scale(-1); break;
    case 4: scale(1); break;
    case 5: scale(0); break;
    }
}
static GtkWidget *button(GtkWidget *bar, const char *label, int action) {
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_box_pack_start(GTK_BOX(bar), b, FALSE, FALSE, 0);
    g_signal_connect(b, "clicked", G_CALLBACK(clicked), GINT_TO_POINTER(action));
    return b;
}
static gboolean key(GtkWidget *w, GdkEventKey *e, gpointer unused) {
    if (e->state & GDK_CONTROL_MASK) {
        switch (e->keyval) {
        case GDK_KEY_o: choose(); return TRUE;
        case GDK_KEY_l: gtk_widget_grab_focus(path); gtk_editable_select_region(GTK_EDITABLE(path), 0, -1); return TRUE;
        case GDK_KEY_plus: case GDK_KEY_equal: scale(1); return TRUE;
        case GDK_KEY_minus: scale(-1); return TRUE;
        case GDK_KEY_0: scale(0); return TRUE;
        case GDK_KEY_b:
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(thumbtoggle),
                                         !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(thumbtoggle)));
            return TRUE;
        }
    }
    if (gtk_widget_has_focus(path) || gtk_widget_has_focus(pagebox)) return FALSE;
    if (e->keyval == GDK_KEY_Page_Down) { move(1); return TRUE; }
    if (e->keyval == GDK_KEY_Page_Up) { move(-1); return TRUE; }
    return FALSE;
}
static gboolean save_page(GdkPixbuf *pixels, const char *filename) {
    gchar *bytes = NULL;
    gsize length = 0;
    if (!gdk_pixbuf_save_to_buffer(pixels, &bytes, &length, "png", NULL, NULL))
        return FALSE;
    gboolean saved = FALSE;
    if (length <= 32768) {
        saved = lx_save_file(filename, bytes, (unsigned)length) >= 0;
    } else {
        FILE *stream = fopen(filename, "wb");
        if (stream) {
            saved = fwrite(bytes, 1, length, stream) == length;
            if (fclose(stream) != 0) saved = FALSE;
        }
    }
    g_free(bytes);
    return saved;
}

static void destroy_window(GtkWidget *widget, gpointer unused) {
    if (save_timer) { g_source_remove(save_timer); save_timer = 0; }
    update_position();
    save_state();
    gtk_main_quit();
}

int main(int argc, char **argv) {
    if (argc == 5 && !strcmp(argv[1], "--render")) {
        const char *err = NULL;
        doc = pdf_open(argv[2], &err);
        if (!doc) { g_printerr("%s\n", err); return 1; }
        int n = atoi(argv[3]); GdkPixbuf *pix = pdf_render(doc, n, 1);
        if (!pix) return 1;
        gboolean ok = save_page(pix, argv[4]);
        if (!ok) g_printerr("无法写入 %s\n", argv[4]);
        g_print("%d/%d %dx%d\n", n, pdf_pages(doc), gdk_pixbuf_get_width(pix), gdk_pixbuf_get_height(pix));
        g_object_unref(pix); pdf_close(doc); return !ok;
    }
    g_setenv("GDK_BACKEND","broadway",TRUE); g_setenv("FONTCONFIG_FILE","/lib/gtk3/fonts.conf",TRUE);
    g_setenv("GSETTINGS_BACKEND","memory",TRUE); g_setenv("NO_AT_BRIDGE","1",TRUE);
    if (!gtk_init_check(&argc,&argv)) return 1;
    load_state();
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window),"PDF 阅读器"); gtk_window_set_decorated(GTK_WINDOW(window),FALSE);
    gtk_window_set_default_size(GTK_WINDOW(window),800,520);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL,4);
    GtkWidget *location = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,4), *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,4);
    gtk_container_add(GTK_CONTAINER(window),box);
    gtk_box_pack_start(GTK_BOX(box),location,FALSE,FALSE,0); gtk_box_pack_start(GTK_BOX(box),bar,FALSE,FALSE,0);
    button(location,"打开…",0); path = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(path),"输入 PDF 路径，按回车打开");
    gtk_box_pack_start(GTK_BOX(location),path,TRUE,TRUE,0);
    g_signal_connect_swapped(path,"activate",G_CALLBACK(open_path),NULL);
    thumbtoggle = gtk_toggle_button_new_with_label("缩略图");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(thumbtoggle), TRUE);
    gtk_box_pack_start(GTK_BOX(location),thumbtoggle,FALSE,FALSE,0);
    g_signal_connect(thumbtoggle,"toggled",G_CALLBACK(toggle_sidebar),NULL);
    back = button(bar,"上一页",1); pagebox = gtk_spin_button_new_with_range(1,1,1);
    gtk_box_pack_start(GTK_BOX(bar),pagebox,FALSE,FALSE,0); forward = button(bar,"下一页",2);
    button(bar,"−",3); button(bar,"+",4); button(bar,"适合宽度",5);
    status = gtk_label_new(""); gtk_box_pack_start(GTK_BOX(bar),status,FALSE,FALSE,0);

    GtkWidget *area = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,4);
    gtk_box_pack_start(GTK_BOX(box),area,TRUE,TRUE,0);
    sidebar = gtk_scrolled_window_new(NULL,NULL);
    gtk_widget_set_size_request(sidebar,150,-1);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sidebar),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC);
    thumbbox = gtk_box_new(GTK_ORIENTATION_VERTICAL,2);
    gtk_container_set_border_width(GTK_CONTAINER(thumbbox),4);
    gtk_container_add(GTK_CONTAINER(sidebar),thumbbox);
    gtk_box_pack_start(GTK_BOX(area),sidebar,FALSE,FALSE,0);
    scrollbox = gtk_scrolled_window_new(NULL,NULL);
    gtk_widget_set_can_focus(scrollbox,TRUE); gtk_box_pack_start(GTK_BOX(area),scrollbox,TRUE,TRUE,0);
    image = gtk_image_new(); gtk_widget_set_valign(image,GTK_ALIGN_START); gtk_container_add(GTK_CONTAINER(scrollbox),image);
    GtkAdjustment *vertical = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrollbox));
    g_signal_connect(vertical,"value-changed",G_CALLBACK(scroll_value_changed),NULL);
    g_signal_connect(vertical,"changed",G_CALLBACK(adjustment_changed),NULL);
    g_signal_connect(pagebox,"value-changed",G_CALLBACK(selected),NULL);
    g_signal_connect(scrollbox,"size-allocate",G_CALLBACK(resize),NULL);
    g_signal_connect(window,"key-press-event",G_CALLBACK(key),NULL);
    g_signal_connect(window,"destroy",G_CALLBACK(destroy_window),NULL);
    gtk_widget_show_all(window);
    if (argc > 1) { gtk_entry_set_text(GTK_ENTRY(path),argv[1]); open_path(); }
    gtk_main(); if (redraw) g_source_remove(redraw); pdf_close(doc); return 0;
}
