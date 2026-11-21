# NetSurf for LixinOS

The optional package installs NetSurf 3.11 as the desktop's **Browser** and as
`/bin/netsurf`. Both launch paths use the upstream HTML parser, CSS layout
engine, framebuffer widgets and image decoders. The framebuffer lives in
userspace. The desktop copies its pixels into a managed window and forwards
pointer and keyboard events. The Rust and TCC kernels run the same ARM64 ELF.

In a QEMU graphics window, run `desktop` and click **Browser**, or start a page
directly from Bash:

```sh
netsurf
netsurf https://example.com/
netsurf /lib/netsurf/welcome.html
```

Press **Ctrl+L**, type a URL and press **Enter**. Click a link to follow it, use
the left arrow on the toolbar for history, and scroll with the mouse wheel.
The title bar controls minimize, maximize/restore and close the window.
**Esc** closes the browser. When launched with `netsurf`, closing its window
returns to the shell. The separate `links` command is also available.

### Build and install on the Mac

Use the project's ARM cross compiler and LixinOS musl runtime. The upstream
build uses host `pkg-config` and libpng for framebuffer assets, and Flex and
Bison 3 to build the bundled nsgenbind DOM binding generator. On macOS the
build checks the Homebrew keg for Bison before using the system tool.
Builds download and verify the pinned NetSurf source bundle, libpng and IJG
JPEG sources. The NetSurf frontend patch and LixinOS adapters are in this directory.

```sh
bash User/netsurf/build.sh
python3 User/gcc/install.py --toolchain netsurf \
  Kernel/target/user-build/netsurf-port/package data/desktop-2026-11-20.5.img
LIXINOS_DISK="$PWD/data/desktop-2026-11-20.5.img" \
  ./scripts/run-kernel.sh --kernel rust --release kernel-2026-11-20.5 --window
```

Choose `--kernel tcc` to use the same disk with the TCC kernel. Shut down the
VM before installing. The installer updates the inactive filesystem snapshot.
It can replace the stock desktop in the verified .5 image or a previous managed
NetSurf installation. It reports a conflict for a locally modified desktop.

### Page support

The port reads local files and fetches HTTP and HTTPS pages and subresources.
It handles relative redirects, checks HTTPS certificates with the guest's
BearSSL transport, and renders CSS, Chinese glyphs, PNG, JPEG, GIF and BMP.
NetSurf measures and draws text at the same CSS pixel size (1–256px).
The guest rasterizes locally generated PingFang and Menlo outlines with
antialiasing. The 16px and 32px sizes keep their native masks. Chinese text
wraps between ideographs and keeps common closing punctuation off the start
of a line and opening punctuation off the end. The desktop uses the same faces.

The framebuffer uses 96 CSS pixels per inch on its logical surface. Descendant
font sizes in `rem` follow the selected root font. A line height declared as
a length or a percentage is resolved before inheritance, while a unitless
multiplier scales with each descendant's font. In a single-line flex column,
an overflowing item can extend beyond the column without widening its
stretched siblings.

The layout engine supports row-major Grid with pixel, percentage and fractional
columns, pixel or fractional rows in a definite-height grid, gaps, column spans and repeated tracks,
including auto-fit/auto-fill with a pixel minimum. Named grid areas, subgrid,
row spans and full intrinsic track sizing are outside this implementation.
The HTML parser handles void `source` elements. On page load, `picture` and
`img` select supported sources using viewport width/height conditions and
`srcset` density or width descriptors. The rendered framebuffer uses one
image sample per logical pixel. A new navigation reselects sources.
The CSS adapter resolves whole-value custom property references for literal
lengths, hex and RGB colours, transparent backgrounds and image URLs declared
in the same stylesheet. It supports `:root`, simple ID and class scopes,
alias chains and media overrides. Complex
scope selectors, cross-sheet bindings and general `var()` expressions still
require broader custom property support. Horizontal logical insets and 2D translations
accept pixel or percentage offsets. This covers common responsive hero banners.
External SVG images and inline SVG in HTML use PlutoSVG and PlutoVG. The
browser rasterizes paths, transforms and gradients at the displayed size and
passes the inherited text colour to inline icons that use `currentColor`.
Background images support `cover` and `contain` with positioning and clipping.
A fully transparent two-stop gradient can overlay a single sprite image.
The framebuffer clears transparent image buffers before drawing. The CSS
colour parser uses the light alternative of `light-dark()`.

Arabic, Hebrew, Devanagari, Tamil, Greek and Cyrillic runs use bundled Noto
fonts and HarfBuzz shaping. Korean syllables use the local Apple SD Gothic Neo
outlines. Adjacent text from HTML character references is measured and shaped
as one run. This supports joining and mark positioning within words. Full
bidirectional layout of mixed-script paragraphs is still outside this port.

The fetcher handles GET requests up to 10 MiB per response. It performs network
I/O synchronously, so a slow server can temporarily delay desktop input.
Keys typed during a transfer are kept for the desktop. Ctrl+C can cancel the
transfer when it is the next key in the input queue.
The port avoids speculative image downloads during HTML parsing. Images from
the constructed box tree enter the fetch queue in document order, allowing
initial layout to proceed before the transfers complete.
Duktape 2.7 runs classic inline and external scripts in the user process.
NetSurf exposes its implemented DOM bindings. After initial layout, an
append at the end of a visible block can add text or a new element subtree
and move later content. General DOM restyling, removal and arbitrary
insertions are incomplete. Modern JavaScript modules, cookie-based
sessions and form POST are outside this build.

Open **JavaScript 排版测试** from the welcome page (or open
`/lib/netsurf/js-demo.html`) to add numbered Chinese paragraphs with the
button. Its external script runs entirely inside the guest.

### Verify

The [Apple China homepage rendering notes](../../docs/netsurf-apple-homepage.md)
include a QEMU screenshot and regression cases for placeholder images, rounded
links, centered Grid buttons and a Flex navigation bar with nested `display: contents`.

The QEMU test boots a disposable copy of the supplied disk, opens the dock
browser, follows a redirect to a local HTTP server and checks actual framebuffer
pixels for an external stylesheet and images. It also checks reflow, wheel
scrolling, a hyperlink, history, and restoring the browser after a terminal exits.
The layout fixture checks fractional columns and gaps, three outline font sizes,
Chinese line breaks, media-based image selection, and a cropped, translated banner.
It also checks SVG gradients, transparent inline icons with inherited colours,
light-scheme backgrounds and the widths of multilingual domain labels written
as HTML character references.
The paint fixture checks scoped colour aliases, background sizing, sprites,
hidden images and explicit image dimensions inside flex containers.

```sh
python3 scripts/test-netsurf.py data/desktop-2026-11-20.5.img --kernel rust
python3 scripts/test-netsurf.py data/desktop-2026-11-20.5.img --kernel tcc
```
