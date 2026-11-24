"""Package the linked Codex executable and its upstream notices for LixinOS."""
import argparse
from pathlib import Path
import runpy
import shutil
from tempfile import TemporaryDirectory

ROOT = Path(__file__).resolve().parents[2]


def package_exec(work, output):
    if output.exists():
        raise FileExistsError(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    executable = work / "target/aarch64-unknown-linux-musl/release/lixinos-codex"
    package_tool = runpy.run_path(str(ROOT / "User/gcc/package.py"))["tool"]
    with TemporaryDirectory(dir=output.parent) as temp:
        package = Path(temp) / "package"
        package_tool(package, "/bin/codex", executable, "/lib/codex", "-DLX_CODEX",
                     work, deflate=True)
        shutil.copytree(work / "source/codex-rs/skills/src/assets/samples",
                        package / "lib/codex/skills")
        for notice in ("LICENSE", "NOTICE"):
            shutil.copyfile(work / "source" / notice, package / "lib/codex" / notice)
        package.rename(output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("work", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    package_exec(args.work.resolve(), args.output.resolve())
