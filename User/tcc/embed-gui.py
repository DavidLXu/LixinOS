"""Build a self-contained fallback for TCC installations without disk headers."""
import json
import sys
from pathlib import Path

header = Path(sys.argv[1])
cells = (header.parent / "cell-font.h").read_text()
unicode = (header.parent / "../links/unicode.h").read_text()
cells = cells.replace('#include "../links/unicode.h"', unicode)
gui = header.read_text().replace('#include "/src/gui/cell-font.h"', cells)
print(json.dumps(gui))
