"""Configure the locked Sunset crate in the build directory for bulk SSH transfers."""
import json
from pathlib import Path
import shutil
import subprocess
import sys

rustup, dest, target = sys.argv[1:]
metadata = json.loads(subprocess.check_output([
    rustup, "run", "stable", "cargo", "metadata", "--locked", "--format-version", "1",
    "--filter-platform", target,
]))
package, = [p for p in metadata["packages"] if p["name"] == "sunset" and p["version"] == "0.6.0"]
source = Path(package["manifest_path"]).parent
dest = Path(dest)
shutil.copytree(source, dest, dirs_exist_ok=True)
config = dest / "src/config.rs"
text = config.read_text()
# The SSH receiver still decodes only one packet at a time in its 35,000-byte
# buffer. Window credit covers a burst of packets before the next adjustment.
for name, value in [("DEFAULT_MAX_PACKET", 32768), ("DEFAULT_WINDOW", 262144)]:
    old = f"pub const {name}: usize = 1000;"
    if text.count(old) != 1:
        raise SystemExit(f"unexpected upstream {name} declaration")
    text = text.replace(old, f"pub const {name}: usize = {value};")
config.write_text(text)
