"""Prepare the upstream Vala sources for the static LixinOS GTK runtime."""
from pathlib import Path
import sys


def replace(text, original, replacement):
    if text.count(original) != 1:
        raise ValueError(f"Unexpected upstream source at {original!r}")
    return text.replace(original, replacement)


def prepare(app, upstream, stage):
    main = f"gnome-{app}.vala"
    text = (upstream / "src" / main).read_text()
    locale = '        Intl.setlocale (LocaleCategory.ALL, "");'
    env = {
        "GDK_BACKEND": "broadway",
        "FONTCONFIG_FILE": "/lib/gtk3/fonts.conf",
        "NO_AT_BRIDGE": "1",
        "GSETTINGS_BACKEND": "keyfile",
        "GSETTINGS_SCHEMA_DIR": f"/lib/{app}/schemas",
    }
    setup = "".join(f'        Environment.set_variable ("{key}", "{value}", true);\n'
                    for key, value in env.items())
    text = replace(text, locale, setup + locale)
    text = replace(text, "flags: ApplicationFlags.FLAGS_NONE", "flags: ApplicationFlags.NON_UNIQUE")
    if app == "tetravex":
        text = replace(text, "headerbar.show_close_button = true;", "headerbar.show_close_button = false;")
    elif app == "mahjongg":
        text = replace(text, "header_bar.set_show_close_button (true);", "header_bar.set_show_close_button (false);")
        view = (upstream / "src/game-view.vala").read_text()
        view = replace(view, "    private Rsvg.Handle? theme_handle = null;\n", "")
        a = view.index("            try {")
        b = view.index("            theme_aspect", a)
        view = view[:a] + "            Gdk.Pixbuf.get_file_info (theme, out theme_width, out theme_height);\n" + view[b:]
        a = view.index("    public void build_pattern ()")
        b = view.index("    public GameView ()", a)
        view = view[:a] + view[b:]
        a = view.index("        if (theme_handle != null)")
        b = view.index("\n    }", a)
        view = view[:a] + """        try {
            var p = new Gdk.Pixbuf.from_file_at_scale (theme, width, height, false);
            Gdk.cairo_set_source_pixbuf (c, p, 0, 0);
            c.paint ();
        } catch (Error error) {
            warning ("Failed to load theme %s: %s", theme, error.message);
        }""" + view[b:]
        (stage / "game-view.vala").write_text(view)
    elif app == "mines":
        text = replace(text, 'headerbar.show_close_button = true;', 'headerbar.show_close_button = false;')
    (stage / main).write_text(text)


if __name__ == "__main__":
    prepare(sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3]))
