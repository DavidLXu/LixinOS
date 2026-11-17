"""Open the QEMU serial console with the project's macOS Terminal profile."""
import os
from pathlib import Path
import plistlib
import shlex
import subprocess
import sys
import tempfile

template = Path(__file__).with_name("LixinOS.terminal")
profile = plistlib.loads(template.read_bytes())
with tempfile.NamedTemporaryFile(prefix="LixinOS-", suffix=".terminal", delete=False) as file:
    # Terminal consumes the profile asynchronously. Remove the disposable copy
    # from the launched command once Terminal has read it.
    # Keep startup errors visible even when Terminal closes finished sessions.
    # Pass paths as arguments so spaces and shell metacharacters stay literal.
    run = """rm -f "$1"
cd "$2" && shift 2 && "$@"
status=$?
if [ "$status" -ne 0 ]; then
    printf '\\nLixinOS 启动或运行失败（退出码 %s）。请查看上方错误。\\n' "$status"
    printf '如果提示磁盘被占用，请先退出使用同一磁盘的 LixinOS。按回车关闭窗口。'
    IFS= read -r answer
fi
exit "$status" """
    profile["CommandString"] = "exec " + shlex.join(
        ["/bin/sh", "-c", run, "lixinos", file.name, os.getcwd(), *sys.argv[1:]]
    )
    profile["RunCommandAsShell"] = False
    plistlib.dump(profile, file)
try:
    subprocess.run(["open", "-a", "Terminal", file.name], check=True)
except (OSError, subprocess.CalledProcessError):
    Path(file.name).unlink(missing_ok=True)
    raise
