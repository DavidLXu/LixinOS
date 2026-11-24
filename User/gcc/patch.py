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

# The guest libc on existing disks derives the six mkstemps characters from
# a coarse realtime clock and a process-local thread ID. Give each live
# compiler and each of its temporary creations a separate filename prefix.
temporary = root / "libiberty/make-temp-file.c"
source = temporary.read_text()
if "cc%d-%u-XXXXXX" not in source:
    start = source.index("make_temp_file (const char *suffix)\n{")
    prefix, body = source[:start], source[start:]
    body = body.replace("  char *temp_filename;", """  char *temp_filename;
#ifdef __lixinos__
  static unsigned sequence;
  char stem[64];
  int stem_len = snprintf (stem, sizeof stem, "cc%d-%u-XXXXXX", (int) getpid (), sequence++);
#else
  const char *stem = TEMP_FILE;
  int stem_len = TEMP_FILE_LEN;
#endif""", 1)
    body = body.replace("+ TEMP_FILE_LEN", "+ stem_len").replace("base_len, TEMP_FILE);",
                                                                "base_len, stem);")
    temporary.write_text(prefix + body)
elif "lx_pid (), sequence++" in source:
    temporary.write_text(source.replace("  extern int lx_pid (void);\n", "")
                         .replace("lx_pid (), sequence++", "(int) getpid (), sequence++"))
