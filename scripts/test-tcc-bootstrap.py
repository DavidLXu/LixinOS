"""Bootstrap two TinyCC generations on a clean guest disk, then replace Bash."""
import runpy
import tempfile
from pathlib import Path

serial = runpy.run_path(str(Path(__file__).with_name("test-user.py")))
prompt = serial["SHELL"]
src = "/src/tinycc/bootstrap.c"


def command(console, line, expected=b""):
    console.send(line.encode() + b"\r")
    output = console.until(prompt, timeout=120)
    assert expected in output, output
    console.expect(b"echo $?\r", prompt, b"\n0\n")


with tempfile.TemporaryDirectory() as directory:
    disk = Path(directory) / "bootstrap.img"
    with serial["boot"](disk) as console:
        command(console, "mkdir build")
        command(console, f"tcc {src} -o build/tcc-new")
        command(console, f"./build/tcc-new {src} -o build/tcc-second")
        command(console, "echo 'int value(void){return 42;}' > lib.c")
        command(console, "echo 'int value(void); int main(void){return value();}' > main.c")
        command(console, "./build/tcc-second lib.c main.c -o probe")
        console.expect(b"./probe\r", prompt)
        console.expect(b"echo $?\r", prompt, b"\n42\n")
        # Distinguish the new login shell from the distribution binary.
        console.expect(b"nano /src/bash/bash.c\r", b"^U Paste line")
        console.expect(b'#define BASH_PROMPT "rebuilt:%s$ "\r\x0f', b"Saved")
        console.expect(b"\x18", prompt)
        command(console, "./build/tcc-second /src/bash/bash.c -o /bin/bash")
        console.expect(b"./build/tcc-second missing.c -o /bin/bash\r", prompt)
        console.expect(b"echo $?\r", prompt, b"\n1\n")
        console.send(b"q\r")
        assert console.vm.wait(timeout=10) == 0
    with serial["boot"](disk, prompt=b"rebuilt:/$ ") as console:
        console.expect(b"echo native-bootstrap\r", b"rebuilt:/$ ", b"\nnative-bootstrap\n")
        console.send(b"q\r")
        assert console.vm.wait(timeout=10) == 0
print("Verified native two-generation TinyCC bootstrap, multi-file linking and reboot into rebuilt Bash.")
