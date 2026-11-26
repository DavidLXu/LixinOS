# LixinOS Code

Run `code` in a graphical LixinOS terminal for a blank document, or pass a UTF-8 text file. A relative path is resolved in the shell's working directory. A new path becomes a file on the first save.

```sh
code notes.txt
code /src/gui
```

A directory opens as a project. Double-click a file in the tree to open a tab. Opening an individual file uses its parent as the search root. **File → Open folder** changes the root while keeping open documents.

| Action | Control |
| --- | --- |
| New, open, save, close current tab | Ctrl+N, Ctrl+O, Ctrl+S, Ctrl+W |
| Undo, redo | Ctrl+Z, Ctrl+Y |
| Quick open by part of a path | Ctrl+P |
| Find or replace in the current document | Ctrl+F, Ctrl+H |
| Search project text and open a matching line | F6 |
| Build the active C file with the guest's `/bin/tcc` | F5 |
| Toggle simplified Chinese pinyin | F7 |

The gutter shows line numbers. C, C++, Rust and Python files have basic syntax highlighting. Enter carries indentation forward and adds a level after an opening brace or colon. Replace All is one undo operation. Project search skips symlinks, hidden entries, binary data and files over 4 MiB, and displays up to 300 matches per query.

Code polls the active file and the visible directory tree. A rename over an existing path reloads the clean document and keeps the cursor near its previous location. If the document has pending edits, the reload dialog defaults to keeping them. Saving over externally changed bytes requires confirmation. A removed file stays available in its tab.

F5 saves pending edits, compiles in the source directory, and writes `name.lxe` beside `name.c` after a successful build. Click a compiler diagnostic, or press Enter on the selected error, to go to the source line. An unsuccessful build retains the previous executable.

### Chinese input

F7 enables pinyin in Code and in FreeCAD built with the LixinOS Qt input adapter. Type a syllable or word, e.g. `nihao`. Space or 1–9 selects a candidate, minus/plus changes page, Backspace corrects the spelling, and Escape cancels the composition. Enter commits the Latin spelling. Code installs the Rime simplified pinyin dictionary and its upstream notices under `/lib/code`. The user-space Qt adapter is in `User/gui/pinyin-qt.h`.

### Shared text clipboard

Code and Lite XL share a guest UTF-8 clipboard of up to 1 MiB. In the shell, pipe text into `clip copy` or redirect `clip paste` into a file. Code also publishes a copy to the Wayland selection. Lite XL's existing desktop copy/paste gestures remain available for short selections.

Graphical sessions enable the Mac text clipboard bridge by default. Start normally:

```sh
./scripts/run-kernel.sh --window
```

Press **F8** to copy the selection to Mac or **F9** to paste from Mac. Both actions are also in the Edit menu. The shell equivalents are `clip copy --mac` and `clip paste --mac`. Guest processes can read or write the Mac text pasteboard through this bridge. Set `LIXINOS_MAC_CLIPBOARD=0` at launch to disable it for that session. The console's Ctrl+Shift+V uses its own 4 KiB bridge.

The document model is Microsoft's MIT-licensed Piece Tree from Monaco Editor 0.52.2. QuickJS runs the bundled model inside the guest. Every text edit updates the tree, and Save writes a snapshot from that tree through Qt's atomic file writer. The adapter tracks UTF-8 BOMs and line endings. Existing mixed line endings are kept on untouched breaks, and new breaks use the first line ending in the file. Qt Widgets supplies the window, editing surface, clipboard and dialogs. The package omits Electron, the VS Code workbench, extensions and AI services. JetBrains Mono is bundled. Code loads the system faces in `/usr/share/fonts`, including the Noto CJK fallback. On older disks it can use Lite XL's bundled CJK font.

The launcher can join an existing Wayland session. From the ordinary graphical shell it starts its own Mutter session and returns to the shell on close. The compositor uses the guest D-Bus, GNOME schema and keyboard-layout data already on the daily disk.

## Build and install

On the Mac, use the ARM64 musl SDK, guest Qt Widgets and Qt Wayland build described in the FreeCAD port, the guest Mutter build and Node/npm. The build pins the Monaco, QuickJS and font archives and the host esbuild version. Node is only used at build time.

```sh
bash User/code/build.sh
python3 User/gcc/install.py --toolchain code \
  Kernel/target/user-build/code-port/guest-package data/shared.img
```

Install on an idle disk. The workspace test uses Tesseract to wait for visible controls and uses isolated pasteboard commands for Mac transfers. The graphical test boots a disposable APFS clone, types through QEMU, saves and reopens the document, and checks the bytes from the guest shell. It also creates a new file and exercises copy and paste over a selection.

```sh
python3 scripts/test-code-editor.py --disk data/shared.img --kernel rust
python3 scripts/test-code-editor.py --disk data/shared.img --kernel tcc
python3 scripts/test-code-workspace.py --disk data/shared.img --kernel rust
python3 scripts/test-lite-xl.py --disk data/shared.img --kernel rust --installed --shared-clipboard
python3 scripts/test-freecad.py --disk data/shared.img --pinyin
```

[Monaco Editor source](https://github.com/microsoft/monaco-editor) · [QuickJS source](https://bellard.org/quickjs/)
