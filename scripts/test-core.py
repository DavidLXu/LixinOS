"""Exercise common filesystem commands, text pipelines and Bash builtins."""
from datetime import datetime, timezone
from pathlib import Path
import re
import runpy
import tempfile

guest = runpy.run_path(str(Path(__file__).with_name("test-user.py")))
ROOT = guest["SHELL"]
WORK = b"lixin:/work$ "


def status(s, value, prompt=WORK):
    s.expect(b"echo $?\r", prompt, f"\n{value}\n".encode())


def check_proc(s):
    def generation():
        s.send(b"sysinfo\r")
        return re.search(rb"committed_snapshot_generation=(\d+)", s.until(ROOT))[1]

    before = generation()
    s.expect(b"ls -1 / | grep proc\r", ROOT, b"\nproc\n")
    s.expect(b"ls -1 /proc\r", ROOT, b"\ncpuinfo\nmeminfo\nstat\nuptime\nversion\n")
    s.expect(b"find /proc -name stat -type f\r", ROOT, b"\n/proc/stat\n")
    s.expect(b"cat < /proc/stat | head -n 1\r", ROOT, b"\ncpu  ")
    s.expect(b"stat /proc/stat\r", ROOT, b"Size: 0 bytes")
    s.expect(b"cat /proc/version\r", ROOT, b"LixinOS ")
    s.expect(b"grep MemAvailable /proc/meminfo\r", ROOT, b"MemAvailable:")
    proc = b"lixin:/proc$ "
    s.expect(b"cd /proc\r", proc)
    s.expect(b"cat cpuinfo\r", proc, b"processor\t: 0")
    s.expect(b"cd ..\r", ROOT)
    s.expect(b"echo overwrite > /proc/stat\r", ROOT, b"read-only filesystem")
    s.expect(b"mkdir /proc/dir\r", ROOT, b"read-only filesystem")
    s.expect(b"touch /proc/file\r", ROOT, b"read-only filesystem")
    s.expect(b"rm -r /proc\r", ROOT, b"read-only filesystem")
    s.expect(b"python\r", guest["PYTHON"])
    for command, output in (
        (b"import os, time", b""),
        (b"""os.chdir('/proc'); print(os.listdir('.'), os.stat('stat')[6])""",
         b"['cpuinfo', 'meminfo', 'stat', 'uptime', 'version'] 0"),
        (b"""f=open('stat'); a=f.readline().split(); time.sleep_ms(60); b=f.readline().split(); f.close()""", b""),
        (b"""c=open('stat').readline().split(); print(len(a), a[3]==b[3] and a[4]==b[4], int(c[3])+int(c[4])>int(a[3])+int(a[4]))""",
         b"\n11 True True\n"),
        (b"""start,idle=open('uptime').read().split(); print(float(start) >= float(idle) >= 0)""", b"\nTrue\n"),
        (b"""open('meminfo', 'w')""", b"OSError"),
    ):
        s.expect(command + b"\r", guest["PYTHON"], output)
    s.expect(b"\x04", ROOT)
    assert generation() == before, "proc access changed the on-disk filesystem"
    s.expect(b"cp /proc/stat cpu-snapshot.txt\r", ROOT)
    s.expect(b"cat cpu-snapshot.txt\r", ROOT, b"\ncpu  ")
    s.expect(b"mv cpu-snapshot.txt /proc/stat\r", ROOT, b"read-only filesystem")
    s.expect(b"mv /proc/stat cpu-moved.txt\r", ROOT, b"read-only filesystem")


def file_times(s, path, prompt):
    s.send(b"stat " + path + b"\r")
    fields = dict(line.split(b": ", 1) for line in s.until(prompt).splitlines() if b": " in line)
    return fields[b"Birth"], fields[b"Modify"]


def exercise(disk):
    with guest["boot"](disk) as s:
        check_proc(s)
        s.expect(b"mkdir -p work/tree/sub work/empty\r", ROOT)
        s.expect(b"cd work\r", WORK)
        for name in ("cp", "mv", "mkdir", "rm", "rmdir", "touch"):
            s.expect(f"""python -c 'print(open("/bin/{name}", "rb").read(4))'\r""".encode(),
                     WORK, b"\nb'LXE1'\n")
        s.expect(b"tcc /src/core/mkdir.c -o ./make-dir\r", WORK)
        s.expect(b"./make-dir -p rebuild/nested\r", WORK)
        s.expect(b"rmdir rebuild/nested rebuild\r", WORK)
        s.expect(b"touch a b\r", WORK)
        s.expect(b"echo listed.txt > manifest\r", WORK)
        s.expect(b"echo nested/invalid.txt >> manifest\r", WORK)
        s.expect(b"touch --files-from manifest\r", WORK, b"not a directory")
        s.expect(b"ls -1 listed.txt\r", WORK, b"path not found")
        s.expect(b"echo listed.txt > manifest\r", WORK)
        s.expect(b"touch --files-from manifest\r", WORK)
        s.expect(b"ls -1 listed.txt\r", WORK, b"\nlisted.txt\n")
        for line in ("pear", "apple", "pear", "banana"):
            s.expect(f"echo {line} >> tree/sub/fruit.txt\r".encode(), WORK)
        s.expect(b"cp -r tree backup\r", WORK)
        s.expect(b"mv backup moved\r", WORK)
        s.expect(b"find moved -name '*.txt' -type f\r", WORK, b"\nmoved/sub/fruit.txt\n")
        s.expect(b"cp a b empty\r", WORK)
        s.expect(b"ls -1 empty\r", WORK, b"\na\nb\n")
        s.expect(b"echo existing > empty/a\r", WORK)
        s.expect(b"cp -n a empty/a\r", WORK)
        s.expect(b"cat empty/a\r", WORK, b"\nexisting\n")
        s.expect(b"rmdir empty\r", WORK, b"directory not empty")
        s.expect(b"rm -f missing/nested\r", WORK)
        status(s, 0)
        s.expect(b"rm -rf /\r", WORK, b"cannot remove the working directory or root")
        s.expect(b"cp -r tree tree/sub/copy\r", WORK, b"overlap")
        s.expect(b"mv moved missing/dir\r", WORK)
        status(s, 1)
        s.expect(b"mkdir -p untouched no-parent/sub\r", WORK)
        s.expect(b"mkdir -p tentative a/sub\r", WORK, b"not a directory")
        s.expect(b"ls -1 tentative\r", WORK, b"path not found")
        s.expect(b"rm empty/a empty/b\r", WORK)
        s.expect(b"rmdir empty\r", WORK)
        s.expect(b"rm -r tree\r", WORK)
        path = b"moved/sub/fruit.txt"
        s.expect(b"head -n 2 " + path + b"\r", WORK, b"\npear\napple\n")
        s.expect(b"tail -2 " + path + b"\r", WORK, b"\npear\nbanana\n")
        s.expect(b"grep apple " + path + b"\r", WORK, b"\napple\n")
        s.expect(b"grep -in PEAR " + path + b"\r", WORK, b"\n1:pear\n3:pear\n")
        s.expect(b"grep -v pear " + path + b"\r", WORK, b"\napple\nbanana\n")
        s.expect(b"grep -rn apple moved\r", WORK, b"\nmoved/sub/fruit.txt:2:apple\n")
        s.expect(b"grep -rl pear moved\r", WORK, b"\nmoved/sub/fruit.txt\n")
        s.expect(b"grep -rc pear moved\r", WORK, b"\nmoved/sub/fruit.txt:2\n")
        s.expect(b"printf 'apple\\n' > moved/binary\r", WORK)
        s.expect(b"python -c 'open(\"moved/binary\", \"ab\").write(bytes([0]))'\r", WORK)
        s.send(b"grep -rn apple moved\r")
        out = s.until(WORK)
        assert b"\nmoved/sub/fruit.txt:2:apple\n" in out and b"binary:" not in out, out
        s.expect(b"rm moved/binary\r", WORK)
        s.expect(b"find moved -maxdepth 1\r", WORK, b"\nmoved\nmoved/sub\n")
        s.expect(b"find moved -mindepth 2 -type f -print\r", WORK, b"\nmoved/sub/fruit.txt\n")
        s.expect(b"tree -L 2 moved\r", WORK, b"|-- fruit.txt")
        s.expect(b"stat " + path + b"\r", WORK, b"Size: 23 bytes")
        s.expect(b"du -sb moved\r", WORK, b"\n23\tmoved\n")
        s.expect(b"cp " + path + b" 'copy with spaces.txt'\r", WORK)
        s.expect(b"cmp -s " + path + b" 'copy with spaces.txt'\r", WORK)
        status(s, 0)
        s.expect(b"sed 's/pear/peach/g' " + path + b" > changed\r", WORK)
        s.expect(b"diff -u " + path + b" changed\r", WORK, b"\n+peach\n")
        status(s, 1)
        s.expect(b"diff -q " + path + b" missing\r", WORK, b"cannot open second file")
        status(s, 2)
        s.expect(b"sed -n '2,3p' changed | nl\r", WORK, b"\n1\tapple\n2\tpeach\n")
        s.expect(b"echo 'red:blue' | cut -d : -f 2 | tr blu BLU\r", WORK, b"\nBLUe\n")
        s.expect(b"seq 2 2 6\r", WORK, b"\n2\n4\n6\n")
        s.expect(b"basename /one/two/\r", WORK, b"\ntwo\n")
        s.expect(b"dirname /one/two/\r", WORK, b"\n/one\n")
        s.expect(b"printf '%s=%d%%\\n' apple -2 banana 4\r", WORK, b"\napple=-2%\nbanana=4%\n")
        s.expect(b"echo abc123 | tr a-z A-Z | tr -d 0-9\r", WORK, b"\nABC\n")
        s.expect(b"test -s " + path + b"\r", WORK)
        status(s, 0)
        s.expect(b"test -f moved\r", WORK)
        status(s, 1)
        s.expect(b"test ! -e missing\r", WORK)
        status(s, 0)
        s.expect(b"find . -maxdepth 1 -name 'copy with spaces.txt' -print0 | xargs -0 cat\r", WORK,
                 b"\npear\napple\npear\nbanana\n")
        s.expect(b"grep absent " + path + b"\r", WORK)
        status(s, 1)
        s.expect(b"sort " + path + b" | uniq -c | tee output\r", WORK,
                 b"\n1 apple\n1 banana\n2 pear\n")
        s.expect(b"cat a output | wc -lwc\r", WORK, b"\n3 6 24\n")
        s.expect(b"echo extra | tee -a output\r", WORK, b"\nextra\n")
        s.expect(b"tail -1 output\r", WORK, b"\nextra\n")
        s.expect(b"sort -ru " + path + b"\r", WORK, b"\npear\nbanana\napple\n")
        s.expect(b"command -v cp\r", WORK, b"\n/bin/cp\n")
        s.expect(b"type cd\r", WORK, b"cd is a shell builtin")
        s.expect(b"history\r", WORK, b"  command -v cp\n")
        s.expect(b"example=abc\r", WORK)
        s.expect(b"unset example\r", WORK)
        s.expect(b'echo "start $example end"\r', WORK, b"\nstart  end\n")
        s.expect(b"cd /\r", ROOT)
        s.expect(b"cd -\r", WORK, b"\n/work\n")
        s.expect(b"true\r", WORK)
        status(s, 0)
        s.expect(b"false\r", WORK)
        status(s, 1)
        s.expect(b"bash -c 'exit 23'\r", WORK)
        status(s, 23)
        s.expect(b"date +%s | wc -l\r", WORK, b"\n1\n")
        s.expect(b"date\r", WORK, b" UTC\n")
        s.expect(b"sleep 0\r", WORK)
        s.expect(b"ps\r", WORK, b"PID PPID STATE MODE")
        s.expect(b"df\r", WORK, b"Filesystem Capacity Used Available")
        s.expect(b"mkdir timed\r", WORK)
        s.expect(b"touch timed/file\r", WORK)
        birth, modified = file_times(s, b"timed/file", WORK)
        assert birth == modified and birth.endswith(b" UTC")
        s.expect(b"sleep 1\r", WORK)
        s.expect(b"echo new > timed/file\r", WORK)
        _, written = file_times(s, b"timed/file", WORK)
        assert written > modified
        assert file_times(s, b"timed/file", WORK) == (birth, written)
        s.expect(b"sleep 1\r", WORK)
        s.expect(b"touch timed/file\r", WORK)
        touched = file_times(s, b"timed/file", WORK)
        assert touched[0] == birth and touched[1] > written
        s.expect(b"mv timed renamed\r", WORK)
        assert file_times(s, b"renamed/file", WORK) == touched
        s.expect(b"ls -l renamed\r", WORK, touched[1] + b"  file")
        s.expect(b"cat renamed/file\r", WORK, b"\nnew\n")
        s.send(b"shutdown\r")
        assert s.vm.wait(timeout=10) == 0

    with guest["boot"](disk) as s:
        assert file_times(s, b"/work/renamed/file", ROOT) == touched
        expected_epoch = int(datetime.fromisoformat(touched[1].decode().removesuffix(" UTC"))
                             .replace(tzinfo=timezone.utc).timestamp())
        s.expect(b"""python -c 'import os; print(os.stat("/work/renamed/file")[8])'\r""",
                 ROOT, f"\n{expected_epoch}\n".encode())
        s.expect(b"ls -l /work/renamed\r", ROOT, touched[1] + b"  file")
        s.expect(b"cat /proc/uptime\r", ROOT)
        s.expect(b"cat /cpu-snapshot.txt\r", ROOT, b"\ncpu  ")
        s.expect(b"cat /work/moved/sub/fruit.txt\r", ROOT, b"\npear\napple\npear\nbanana\n")
        s.expect(b"cat /work/output\r", ROOT, b"\n1 apple\n1 banana\n2 pear\nextra\n")
        s.expect(b"ls -1 /work/tree\r", ROOT, b"path not found")
        s.send(b"shutdown\r")
        assert s.vm.wait(timeout=10) == 0


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as tmp:
        exercise(Path(tmp) / "core.img")
    print("Verified common Bash commands, pipelines, stable filesystem changes and reboot.")
