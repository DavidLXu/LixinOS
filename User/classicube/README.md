# ClassiCube on LixinOS

Run `classicube` in the graphical LixinOS shell to generate a classic creative
world. Use `classicube --flat` to start with a flat grass world for building.

| Control | Action |
| --- | --- |
| W, A, S, D | Walk |
| Space | Jump |
| Mouse or arrow keys | Look around |
| Left / right mouse button | Break / place the targeted block |
| 1–9 or scroll wheel | Select a hotbar slot |
| B | Open the block palette |
| Esc | Pause or resume |
| Ctrl+Q | Quit to the shell |

Use **Esc → Save level** before quitting to keep a world. The client stores
maps and settings under `ClassiCube/` in the directory where you launch it.
Start from the same directory and use **Esc → Load level** to reopen a map.

## Build and install on Apple Silicon

From the repository root, build the ARM64 ELF and install it on an idle work
disk. The package installer updates the inactive filesystem snapshot.

```sh
bash User/classicube/build.sh
python3 User/gcc/install.py --toolchain classicube \
  Kernel/target/user-build/classicube-port/package \
  "$HOME/Library/Application Support/LixinOS/daily.img"
./scripts/run-kernel.sh --kernel rust --window
```

The build pins upstream revision
`74345260e5af87ba0b850a2048399864e38f33c8` and packages its textures,
license and credits under `/lib/classicube`. The game uses the upstream world
generator, collision detection, block editing and software 3D renderer. The
LixinOS backend presents frames to the foreground canvas and translates
tablet events and console keystrokes into game input. Keyboard movement uses
short presses renewed by key repeat because the console delivers bytes.
Approaching the left or right edge of the tablet continues turning.
This port targets offline single-player play with sound disabled.

The QEMU test takes a disposable copy of the supplied LixinOS disk. It sends
keyboard and tablet events and checks the rendered frame, player coordinates,
block changes and return to the shell. Run it with either kernel.

```sh
python3 scripts/test-classicube.py --kernel rust --disk "$HOME/Library/Application Support/LixinOS/daily.img"
python3 scripts/test-classicube.py --kernel tcc  --disk "$HOME/Library/Application Support/LixinOS/daily.img"
```

[Upstream ClassiCube](https://github.com/ClassiCube/ClassiCube)
