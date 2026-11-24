#include <gtk/gtk.h>
#include <garcon/garcon.h>
#include <unistd.h>
#include <stdio.h>
#include "panel/panel-itembar.h"
#include "plugins/clock/clock-digital.h"
#include "session.h"
#include "xfce-backdrop.h"

static void size_backdrop(GtkWidget *area, GtkAllocation *size, gpointer data) {
    (void)area;
    XfceBackdrop *backdrop = data;
    xfce_backdrop_set_size(backdrop, size->width, size->height);
    GdkPixbuf *cached = xfce_backdrop_get_pixbuf(backdrop);
    if (cached) g_object_unref(cached);
    else xfce_backdrop_generate_async(backdrop);
}

static gboolean draw_backdrop(GtkWidget *area, cairo_t *cr, gpointer data) {
    (void)area;
    GdkPixbuf *pixbuf = xfce_backdrop_get_pixbuf(data);
    if (pixbuf) {
        gdk_cairo_set_source_pixbuf(cr, pixbuf, 0, 0);
        cairo_paint(cr);
        g_object_unref(pixbuf);
    }
    return FALSE;
}

/* Frame actions so the host can skip unrelated toolkit diagnostics on stderr. */
static void action(GtkButton *button, gpointer data) {
    (void)button;
    char *line = g_strdup_printf(XFCE_RECORD "XFCE %s\n", (char *)data);
    write(STDERR_FILENO, line, strlen(line));
    g_free(line);
}

static GtkWidget *button(const char *title, const char *command) {
    GtkWidget *item = gtk_button_new_with_label(title);
    g_object_set_data_full(G_OBJECT(item), "command", g_strdup(command), g_free);
    g_signal_connect(item, "clicked", G_CALLBACK(action),
                     g_object_get_data(G_OBJECT(item), "command"));
    return item;
}

static void add_app(GtkFlowBox *apps, GHashTable *added, const char *path, const char *label) {
    if (!g_str_has_prefix(path, "/bin/") || !window_name(path + 5) ||
        !g_file_test(path, G_FILE_TEST_IS_REGULAR) || g_hash_table_contains(added, path)) return;
    g_hash_table_add(added, g_strdup(path));
    GtkWidget *item = button(label, path);
    gtk_widget_set_size_request(item, 148, 44);
    gtk_flow_box_insert(apps, item, -1);
}

static int by_name(const void *a, const void *b) {
    return g_ascii_strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

static void populate(GtkFlowBox *apps) {
    GHashTable *added = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GarconMenu *menu = garcon_menu_new_for_path("/lib/xfce/applications.menu");
    if (garcon_menu_load(menu, NULL, NULL)) {
        GList *items = garcon_menu_get_items(menu);
        for (GList *p = items; p; p = p->next) {
            GarconMenuItem *item = p->data;
            const char *command = garcon_menu_item_get_command(item);
            if (command) add_app(apps, added, command, garcon_menu_item_get_name(item));
        }
        g_list_free(items);
    }
    g_object_unref(menu);
    /* Guest-compiled window apps appear even without a freedesktop entry. */
    GDir *dir = g_dir_open("/bin", 0, NULL);
    if (dir) {
        GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
        const char *name;
        while ((name = g_dir_read_name(dir)))
            if (window_name(name)) g_ptr_array_add(names, g_strdup(name));
        g_dir_close(dir);
        g_ptr_array_sort(names, by_name);
        for (guint i = 0; i < names->len; ++i) {
            name = g_ptr_array_index(names, i);
            char *path = g_build_filename("/bin", name, NULL);
            char *label = g_strndup(name, strcspn(name, "."));
            add_app(apps, added, path, label);
            g_free(path);
            g_free(label);
        }
        g_ptr_array_unref(names);
    }
    g_hash_table_unref(added);
}

int main(int argc, char **argv) {
    g_setenv("GDK_BACKEND", "broadway", TRUE);
    g_setenv("FONTCONFIG_FILE", "/lib/gtk3/fonts.conf", TRUE);
    g_setenv("GSETTINGS_BACKEND", "memory", TRUE);
    g_setenv("NO_AT_BRIDGE", "1", TRUE);
    if (!gtk_init_check(&argc, &argv)) return 1;
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Xfce");
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), box);
    GtkWidget *panel = panel_itembar_new();
    g_object_set(panel, "size", (guint)PANEL_HEIGHT, NULL);
    GtkWidget *strip = gtk_event_box_new();
    gtk_widget_set_name(strip, "panel");
    gtk_container_add(GTK_CONTAINER(strip), panel);
    gtk_box_pack_start(GTK_BOX(box), strip, FALSE, FALSE, 0);
    const char *labels[] = {"Applications", "Files", "Next window", "Close window", "Log out"};
    const char *commands[] = {"home", "/bin/thunar.win3", "next", "close", "logout"};
    for (unsigned i = 0; i < G_N_ELEMENTS(labels); ++i)
        panel_itembar_insert(PANEL_ITEMBAR(panel), button(labels[i], commands[i]), -1);
    GtkWidget *space = gtk_label_new(" ");
    panel_itembar_insert(PANEL_ITEMBAR(panel), space, -1);
    gtk_container_child_set(GTK_CONTAINER(panel), space, "expand", TRUE, NULL);
    ClockTime *time = clock_time_new();
    char zone[64] = "Asia/Shanghai";
    FILE *prefs = fopen("/.desktop-boot", "r");
    if (prefs) {
        if (fscanf(prefs, "%*s %63s", zone) != 1) strcpy(zone, "Asia/Shanghai");
        fclose(prefs);
    }
    g_setenv("TZDIR", "/lib/xfce/zoneinfo", TRUE);
    if (zone[0] == '/' || strstr(zone, "..")) strcpy(zone, "UTC");
    g_object_set(time, "timezone", zone, NULL);
    GtkWidget *clock = xfce_clock_digital_new(time, NULL);
    g_object_set(clock, "digital-time-font", "Sans 12", NULL);
    gtk_widget_set_margin_end(clock, 12);
    panel_itembar_insert(PANEL_ITEMBAR(panel), clock, -1);
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_container_set_border_width(GTK_CONTAINER(body), 22);
    GtkWidget *overlay = gtk_overlay_new();
    gtk_box_pack_start(GTK_BOX(box), overlay, TRUE, TRUE, 0);
    GtkWidget *wall = gtk_drawing_area_new();
    gtk_container_add(GTK_CONTAINER(overlay), wall);
    XfceBackdrop *backdrop = xfce_backdrop_new(gtk_widget_get_visual(window));
    xfce_backdrop_set_image_style(backdrop, XFCE_BACKDROP_IMAGE_ZOOMED);
    g_signal_connect_swapped(backdrop, "ready", G_CALLBACK(gtk_widget_queue_draw), wall);
    g_signal_connect(wall, "size-allocate", G_CALLBACK(size_backdrop), backdrop);
    g_signal_connect(wall, "draw", G_CALLBACK(draw_backdrop), backdrop);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), body);
    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<span foreground='white' size='xx-large' weight='bold'>Xfce</span>   <span foreground='white'>Applications</span>");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(body), title, FALSE, FALSE, 0);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_box_pack_start(GTK_BOX(body), scroll, TRUE, TRUE, 0);
    GtkWidget *apps = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(apps), GTK_SELECTION_NONE);
    gtk_widget_set_valign(apps, GTK_ALIGN_START);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(apps), 5);
    gtk_container_add(GTK_CONTAINER(scroll), apps);
    populate(GTK_FLOW_BOX(apps));
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "window { background: #e6edf4; color: #192f47; }"
        "#panel { background: #273952; color: white; }"
        "#panel button { background: #273952; color: white; border: none; border-radius: 0; }"
        "#panel button:hover { background: #385774; }", -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    gtk_widget_show_all(window);
    g_object_set(clock, "digital-layout", (guint)CLOCK_PLUGIN_DIGITAL_FORMAT_TIME,
                 "digital-time-format", "%H:%M %Z", NULL);
    gtk_main();
    g_object_unref(backdrop);
    g_object_unref(time);
    g_object_unref(css);
    return 0;
}
