"""Select the LixinOS target defaults and native process backend for GCC."""
from pathlib import Path
import shutil
import sys

root = Path(sys.argv[1])
port = Path(__file__).resolve().parent

def copy(src, dest):
    if not dest.exists() or src.read_bytes() != dest.read_bytes():
        shutil.copyfile(src, dest)
        return True
    return False

backend = root / "libiberty/pex-unix.c"
source = backend.read_text()
if not source.startswith("#ifdef __lixinos__"):
    backend.write_text('#ifdef __lixinos__\n#include "pex-lixin.inc"\n#else\n' + source + '\n#endif\n')
pex_changed = copy(port/"pex.inc", backend.with_name("pex-lixin.inc"))
console_changed = copy(port.parent/"console.h", backend.with_name("console.h"))
if pex_changed or console_changed:
    backend.touch()

target = root/"gcc/config/aarch64/aarch64-elf-raw.h"
original = target.read_text()
marker = "/* LixinOS target defaults */"
base = original.partition(marker)[0].removesuffix('#include "lixinos.h"\n')
updated = base.rstrip() + "\n\n" + marker + "\n" + (port/"lixinos.h").read_text()
if updated != original:
    target.write_text(updated)
