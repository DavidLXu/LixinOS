import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

const file = Gio.File.new_for_path('/lib/gnome/gjs-check.js');
if (GLib.path_get_basename(file.get_path()) !== 'gjs-check.js')
    throw new Error('Unexpected path from GLib and Gio');
if (typeof imports.gettext.gettext('LixinOS') !== 'string')
    throw new Error('GjsPrivate gettext binding returned an unexpected value');
print('GJS called GLib, Gio, and GjsPrivate');

let called = false;
GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
    called = true;
    return GLib.SOURCE_REMOVE;
});
GLib.MainContext.default().iteration(false);
if (!called)
    throw new Error('GLib did not call JavaScript');
print('GLib called JavaScript through libffi');
