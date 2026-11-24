# Doom engine on LixinOS

Run `doom` in the graphical LixinOS shell. The default game is Freedoom
Phase 2, using the upstream DoomGeneric engine and the original Freedoom
levels. Use `doom -warp 1` to go straight to the first map. To use another
compatible IWAD, pass `doom -iwad /path/to/game.wad`.

| Control | Action |
| --- | --- |
| W / S or Up / Down | Move forward / backward |
| A / D | Move sideways |
| Left / Right | Turn |
| F or left click | Fire |
| Space | Open a door or activate a switch |
| 1–7 | Select weapon |
| Tab | Toggle the map |
| Esc | Open the menu |
| Ctrl+Q | Return to the shell |

The menu uses arrow keys and Enter. Save and load are in the game menu.
Configuration and saves go under `.doom/` in the launch directory. This
port uses the foreground canvas, software rendering and local single-player
input. Sound and music playback are pending.

## Build on Apple Silicon

```sh
bash User/doom/build.sh
python3 User/gcc/install.py --toolchain doom \
  Kernel/target/user-build/doom-port/package \
  "$HOME/Library/Application Support/LixinOS/daily.img"
```

The working disk must be idle during installation. The package includes
Freedoom Phase 2 0.12.1, credits and both licenses. The build checks the source
and data archives against fixed SHA-256 hashes.

The graphical regression starts MAP01 on a disposable copy of the input
disk. It checks the rendered colors and reads back the gameplay recording
to verify that forward movement, turning and firing reached the engine.

```sh
python3 scripts/test-doom.py --disk data/desktop-2026-11-20.5.img \
  --kernel rust --release kernel-2026-11-21.25
python3 scripts/test-doom.py --disk data/desktop-2026-11-20.5.img \
  --kernel tcc --release kernel-2026-11-21.25
```

[DoomGeneric upstream](https://github.com/ozkl/doomgeneric) ·
[Freedoom upstream](https://github.com/freedoom/freedoom)
