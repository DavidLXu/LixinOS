#include <gtk/gtk.h>

static void show_about(GtkButton *button, gpointer data)
{
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(data), GTK_DIALOG_MODAL |
        GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE,
        "GTK %u.%u.%u on LixinOS", gtk_get_major_version(), gtk_get_minor_version(),
        gtk_get_micro_version());
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
        "这个窗口由 GTK 和 Pango 绘制。");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 380, -1);
    gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

int main(int argc, char **argv)
{
    g_setenv("GDK_BACKEND", "broadway", TRUE);
    g_setenv("GSETTINGS_BACKEND", "memory", TRUE);
    g_setenv("NO_AT_BRIDGE", "1", TRUE);
    g_setenv("FONTCONFIG_FILE", "/lib/gtk3/fonts.conf", TRUE);
    if (!gtk_init_check(&argc, &argv)) return 1;

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "GTK 3 Demo");
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(window), 720, 436);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 18);
    gtk_container_add(GTK_CONTAINER(window), box);

    GtkWidget *label = gtk_label_new("GTK 3 控件测试");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget *text = gtk_text_view_new();
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(text)),
        "This text uses GtkTextView.\n你可以在这里输入或粘贴中文。", -1);
    gtk_container_add(GTK_CONTAINER(scroller), text);
    gtk_box_pack_start(GTK_BOX(box), scroller, TRUE, TRUE, 0);
    GtkWidget *button = gtk_button_new_with_label("About GTK");
    gtk_widget_set_halign(button, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
    g_signal_connect(button, "clicked", G_CALLBACK(show_about), window);

    gtk_widget_show_all(window);
    gtk_main();
    return 0;
}
