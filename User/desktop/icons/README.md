# Launcher icon artwork

`launcher-icons.py` composes these 192x192 RGBA images into the binary icon
bank read by the desktop from `/lib/netsurf/launcher-icons.bin`.  The launcher
and dock have separate pre-rendered sizes, so the guest never scales or decodes
an image.

Most icons come from the **Papirus icon theme** (GPL-3.0, see `COPYING`),
rasterised from the 48x48 SVG sources:

| file | Papirus source |
| --- | --- |
| folder.png | `places/folder-orange.svg` |
| calc.png | `apps/accessories-calculator.svg` |
| terminal.png | `apps/utilities-terminal.svg` |
| sheet.png | `apps/accessories-text-editor.svg` |
| editor.png | `apps/mousepad.svg` |
| store.png | `apps/org.gnome.Software.svg` |
| settings.png | `apps/org.gnome.Settings.svg` |
| tasks.png | `apps/utilities-system-monitor.svg` |
| tiles.png | `apps/gnome-tetravex.svg` |
| video.png | `apps/multimedia-video-player.svg` |
| calendar.png | `apps/org.gnome.Calendar.svg` |
| maps.png | `apps/org.gnome.Maps.svg` |
| clock.png | `apps/org.gnome.clocks.svg` |
| photo.png | `apps/org.gnome.eog.svg` |
| mines.png | `apps/gnome-mines.svg` |
| mahjong.png | `apps/gnome-mahjongg.svg` |
| eyes.png | `apps/xeyes.svg` |
| code.png | `apps/code.svg` |
| widgets.png | `apps/gtk3-demo.svg` |
| engineering.png | `apps/applications-engineering.svg` |
| writer.png | `apps/libreoffice-writer.svg` |
| pdf.png | `mimetypes/application-pdf.svg` |
| disk.png | `devices/drive-harddisk.svg` |
| git.png | `apps/git.svg` |
| python.png | `apps/python.svg` |
| doom.png | `apps/doom.svg` |
| classicube.png | `apps/minecraft.svg` |
| freecad.png | `apps/freecad.svg` |

`browser.png` (globe), `physics.png` (atom symbol), `paint.png` (artist
palette) and `nfs.png` (automobile) are **Fluent Emoji** Color artwork, MIT
licensed (see `FLUENT-LICENSE`), from
<https://github.com/microsoft/fluentui-emoji>.

`node.png` and `codex.png` recolour the Node.js and OpenAI marks from
[Simple Icons](https://simpleicons.org/) (CC0).  `deepseek.png` recolours the
project's `User/dsh/gui/deepseek.svg` whale for a launcher badge.  The remaining
artwork (`apps.png`, `counter.png`, `game2048.png`, `memory.png`, `radar.png`,
`tank.png`, `yuri.png`) was drawn for LixinOS.
