#include <ibus.h>
#include <stdio.h>

static void commit(IBusInputContext *context, IBusText *text, gpointer unused) {
    fputs(ibus_text_get_text(text), stdout);
    fflush(stdout);
}
int main(int argc, char **argv) {
    if (argc < 2) { fputs("usage: ibus-input KEYSYM [KEYSYM ...]\n", stderr); return 2; }
    ibus_init();
    IBusBus *bus = ibus_bus_new();
    if (!ibus_bus_is_connected(bus)) { fputs("cannot connect to ibus-daemon\n",stderr); return 1; }
    gboolean activate = argc == 2 && !g_strcmp0(argv[1], "--activate");
    if (activate) {
        for (unsigned i = 0; i < 100 && !ibus_bus_name_has_owner(bus, "org.freedesktop.IBus.Simple"); ++i)
            g_usleep(20000);
    }
    IBusInputContext *ctx = ibus_bus_create_input_context(bus,"ibus-input");
    if (!ctx) return 1;
    ibus_input_context_set_capabilities(ctx,IBUS_CAP_FOCUS | IBUS_CAP_PREEDIT_TEXT);
    g_signal_connect(ctx,"commit-text",G_CALLBACK(commit),NULL);
    ibus_input_context_focus_in(ctx);
    if (!ibus_bus_set_global_engine(bus,"xkb:us::eng")) {
        fputs("cannot select xkb:us::eng\n",stderr); return 1;
    }
    for (int i=1; !activate && i<argc; ++i) {
        guint key=ibus_keyval_from_name(argv[i]);
        if (!key || key==IBUS_KEY_VoidSymbol) return 2;
        if (!ibus_input_context_process_key_event(ctx,key,0,0)) {
            gunichar cp=ibus_keyval_to_unicode(key);
            if (cp) { char bytes[8]; gint n=g_unichar_to_utf8(cp,bytes); fwrite(bytes,1,n,stdout); }
        }
        ibus_input_context_process_key_event(ctx,key,0,IBUS_RELEASE_MASK);
        while (g_main_context_iteration(NULL,FALSE)) {}
    }
    ibus_input_context_focus_out(ctx);
    while (g_main_context_iteration(NULL,FALSE)) {}
    if (!activate) puts("");
    g_object_unref(ctx);
    g_object_unref(bus);
    return 0;
}
