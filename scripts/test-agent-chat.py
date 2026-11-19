"""Verify the foreground chat, guest commands, approvals and live output in QEMU."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import time

here = Path(__file__).resolve().parent


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, here / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


serial = load("serial", "test-user.py")
bridge = load("bridge", "agent-bridge.py")
PROMPT = b"\x1b[96magent> \x1b[0m"
APPROVAL = b"[y/N] "
STAGED_CODE = "# first line\n# second line\n# third line\n# beyond the preview\nprint(42)\n"
PREVIEW = b"# first line\n# second line\n# third line\n[more lines hidden]"
COMMAND_COLOR = b"\x1b[36m"
REPLY_COLOR = b"\x1b[95m"
COLOR_RESET = b"\x1b[0m"


def reply(instruction, data):
    assert "LixinOS foreground chat agent" in instruction
    assert data["workspace"] == "/work"
    records = data["observations"]
    assert all(isinstance(record, dict) and {"tool", "status", "detail"} <= record.keys()
               for record in records), records
    observations = [f'tool "{record["tool"]}": {record["detail"]}' for record in records]
    task = data["task"]
    assert 'exec is your shell command tool' in instruction
    if task == "Hello":
        assert not records
        time.sleep(0.55)
        return {"tool": "finish", "args": ["Hello!"]}
    if task == "Check test timeout status":
        if not records:
            return {"tool": "write", "args": ["hang.py", "while True: pass\n"]}
        if len(records) == 1:
            return {"tool": "test", "args": ["hang.py", "", ""]}
        assert records[-1]["status"] == "timed_out" and "exit_code" not in records[-1], records
        # The failed candidate is never offered for save approval.
        return {"tool": "finish", "args": ["The timed-out candidate was saved."]}
    if task == "Create 100 random number files":
        if not observations:
            code = (
                "import os, random\n"
                "assert os.getcwd() == '/work'\n"
                "for i in range(100):\n"
                "    with open('num_%03d.txt' % i, 'w') as f:\n"
                "        f.write(str(random.randint(0, 999999)) + '\\n')\n"
                "print(len([name for name in os.listdir() if name.startswith('num_')]))"
            )
            return {"tool": "exec", "args": ["python", "-c", code]}
        assert "100" in observations[-1] and "status 0" in observations[-1], observations
        return {"tool": "finish", "args": ["Created 100 random number files."]}
    if task == "Run a shell pipeline":
        if not observations:
            return {"tool": "exec", "args": ["bash", "-c", "echo pipeline | cat > shell.txt"]}
        assert "status 0" in observations[-1], observations
        return {"tool": "finish", "args": ["The shell pipeline finished."]}
    if task == "Verify staged tests cannot write files":
        if not observations:
            source = ("for action in (lambda: open('escaped.txt', 'w'), "
                      "lambda: __import__('os').system('touch escaped.txt')):\n"
                      "    try: action()\n"
                      "    except OSError: print('blocked')\n")
            return {"tool": "write", "args": ["isolated.py", source]}
        if len(observations) == 1:
            return {"tool": "test", "args": ["isolated.py", "", "blocked\nblocked\n"]}
        assert "test passed" in observations[-1], observations
        return {"tool": "finish", "args": ["Staged test writes were blocked."]}
    if task == "Build and test Snake":
        verify = {"tool": "verify", "args": ["snake.py", "python /work/snake.py", "interactive"]}
        if not observations:
            source = (here.parent / "User/games/snake.py").read_text()
            return {"tool": "write", "args": ["snake.py", source]}
        if len(observations) == 1:
            assert records[-1]["status"] == "staged" and records[-1]["path"] == "/work/snake.py", records
            return verify
        if len(observations) == 2:
            assert records[-1]["status"] == "staged" and "artifact" not in records[-1], records
            return {"tool": "apply", "args": []}
        if len(observations) == 3:
            assert records[-1]["status"] == "needs_tests", records
            return {"tool": "test", "args": [
                "snake.py", "q", "Snake ready. Arrows or WASD to start, Q to quit.\nBye.\n",
            ]}
        if len(observations) == 4:
            assert records[-1]["status"] == "test_passed" and records[-1]["exit_code"] == 0, records
            return {"tool": "apply", "args": []}
        if len(observations) == 5:
            assert records[-1]["status"] == "saved", records
            return {"tool": "smoke", "args": ["snake.py", "\x1b[Aq", "Arrows or WASD to move"]}
        if len(observations) == 6:
            assert records[-1]["status"] == "test_passed" and records[-1]["exit_code"] == 0, records
            return verify
        receipt = records[-1]["artifact"]
        assert records[-1]["status"] == "verified" and records[-1]["path"] == "/work/snake.py", records
        assert receipt["checks"] == {"exact_passed": 1, "smoke_passed": 1, "failed": 0}, receipt
        assert receipt["interactive"] == "scripted" and receipt["snapshot"] > 0, receipt
        return {"tool": "finish", "args": ["Snake is ready. Start it with python /work/snake.py."]}
    if task in ("Stage a file and keep it", "Stage a file and discard it"):
        name = "kept.py" if task.endswith("keep it") else "discarded.py"
        if not observations:
            return {"tool": "write", "args": [name, STAGED_CODE]}
        if name == "kept.py" and len(observations) == 1:
            return {"tool": "read", "args": [name]}
        if not observations[-1].startswith('tool "test"'):
            if name == "kept.py":
                assert STAGED_CODE in observations[-1], observations
            return {"tool": "test", "args": [name, "", "42\n"]}
        assert "test passed" in observations[-1], observations
        return {"tool": "finish", "args": ["The candidate passed its test."]}
    if task == "Read the staged script from disk":
        if not observations:
            return {"tool": "exec", "args": ["cat", "kept.py"]}
        assert STAGED_CODE in observations[-1], observations
        return {"tool": "finish", "args": ["Read the full file."]}
    if task == "Please list the workspace":
        if not observations:
            return {"tool": "exec", "args": ["ls", "-1", "."]}
        assert "note.txt" in observations[-1] and "status 0" in observations[-1]
        return {"tool": "finish", "args": ["I found note.txt."]}
    if task == "Find the file containing hello-chat":
        assert 'grep -rn' in instruction
        if not observations:
            return {"tool": "exec", "args": ["find", ".", "-maxdepth", "1", "-name", "note.txt"]}
        if len(observations) == 1:
            assert "./note.txt" in observations[-1] and "status 0" in observations[-1], observations
            return {"tool": "exec", "args": ["grep", "-n", "hello-chat", "note.txt"]}
        assert "1:hello-chat" in observations[-1] and "status 0" in observations[-1], observations
        return {"tool": "finish", "args": ["The match is note.txt line 1."]}
    if task == "How is my CPU?":
        assert "For CPU questions, run cpuinfo once." in instruction
        if not observations:
            return {"tool": "exec", "args": ["cpuinfo"]}
        assert len(observations) == 1 and "status 0" in observations[0], observations
        fields = dict(line.split("=", 1) for line in observations[0].splitlines() if "=" in line)
        percent = int(fields["guest_busy_percent"])
        assert 0 <= percent <= 100 and int(fields["sample_ms"]) >= 250, observations
        return {"tool": "finish", "args": [f"Measured guest CPU usage was {percent} percent."]}
    if task == "Read CPU counters from proc":
        assert "/proc/stat" in instruction
        if not observations:
            return {"tool": "exec", "args": ["cat", "/proc/stat"]}
        rows = [line.split() for line in observations[0].splitlines() if line.startswith("cpu")]
        assert len(rows) == 2 and rows[0][0] == "cpu" and rows[1][0] == "cpu0", observations
        assert len(rows[0]) == 11 and rows[0][1:] == rows[1][1:], observations
        return {"tool": "finish", "args": ["Read live counters from /proc/stat."]}
    if task == "Check identity before a model error":
        if not observations:
            return {"tool": "exec", "args": ["uname", "-a"]}
        assert "LixinOS" in observations[0], observations
        raise bridge.BridgeError("DeepSeek HTTP 401")
    if task == "What did you learn before that error?":
        assert "Turn interrupted:" in data["conversation"]
        assert "LixinOS 0.1.0" in data["conversation"]
        return {"tool": "finish", "args": ["The guest is LixinOS."]}
    if task == "Recover from a malformed reply":
        if not observations:
            return {"tool": "exec", "args": ["uname", "-a"]}
        assert len(observations) == 1 and "LixinOS" in observations[0], observations
        if "Regenerate your reply" not in instruction:
            return {"tool": "finish", "args": {"answer": "invalid argument shape"}}
        return {"tool": "finish", "args": ["Recovered without running uname again."]}
    if task == "Read the shipped build guide":
        if not observations:
            return {"tool": "exec", "args": ["cat", "/src/build/README.md"]}
        assert "TinyCC" in observations[0] and "./rebuild" in observations[0], observations
        return {"tool": "finish", "args": ["The build guide was available."]}
    if task == "Create a marker automatically":
        assert "Auto-approval is enabled" in instruction
        if not observations:
            return {"tool": "exec", "args": ["touch", "auto-ok.txt"]}
        assert "status 0" in observations[0], observations
        if len(observations) == 1:
            return {"tool": "exec", "args": ["printf", "marker output"]}
        assert "marker output" in observations[1], observations
        if len(observations) == 2:
            return {"tool": "verify", "args": ["auto-ok.txt"]}
        assert records[-1]["status"] == "verified", records
        return {"tool": "finish", "args": ["Marker created."]}
    if task == "Modify a verified file":
        if not observations:
            return {"tool": "exec", "args": ["bash", "-c", "echo before > stale.txt"]}
        if len(observations) == 1:
            return {"tool": "verify", "args": ["stale.txt", "cat /work/stale.txt", "interactive"]}
        if len(observations) == 2:
            assert records[-1]["status"] == "verified", records
            assert records[-1]["artifact"]["interactive"] == "pending", records
            return {"tool": "exec", "args": ["bash", "-c", "echo changed > stale.txt"]}
        if len(observations) == 3:
            return {"tool": "finish", "args": ["This stale receipt should never be shown."]}
        if len(observations) == 4:
            assert records[-1]["status"] == "stale" and records[-1]["path"] == "/work/stale.txt", records
            return {"tool": "verify", "args": ["stale.txt"]}
        assert records[-1]["status"] == "verified", records
        return {"tool": "finish", "args": ["Verified the updated file."]}
    if task.startswith("Open local SSH "):
        port = task.rsplit(" ", 1)[1]
        args = ["ssh", "-p", port, "guest@10.0.2.2"]
        if not observations:
            return {"tool": "exec", "args": ["ssh", "--help"]}
        if len(observations) == 1:
            assert "usage: ssh" in observations[0], observations
            return {"tool": "exec", "args": args}
        if len(observations) == 2:
            assert records[-1]["status"] == "requires_terminal" and "exit_code" not in records[-1], records
            return {"tool": "exec", "args": ["bash", "-c", " ".join(args)]}
        if len(observations) == 3:
            assert records[-1]["status"] == "requires_terminal" and records[-1]["exit_code"] == 1, records
            return {"tool": "terminal", "args": args}
        assert records[-1]["status"] == "ok" and records[-1]["exit_code"] == 0, records
        assert "remote$" not in observations[-1], observations
        return {"tool": "finish", "args": ["SSH terminal closed successfully."]}
    if task == "Repeat the failed command":
        if len(observations) in (1, 2):
            assert "status 1" in observations[-1], observations
        if len(observations) == 2:
            assert "Skipped repeated failed command" in observations[-1], observations
            return {"tool": "exec", "args": ["pwd"]}
        if len(observations) == 3:
            assert "status 0" in observations[-1] and "/work" in observations[-1], observations
        return {"tool": "exec", "args": ["false"]}
    if task == "Retry after changing the workspace":
        args = ["test", "-f", "retry.txt"]
        if not observations:
            return {"tool": "exec", "args": args}
        if len(observations) == 1:
            assert "status 1" in observations[-1], observations
            return {"tool": "exec", "args": ["touch", "retry.txt"]}
        if len(observations) == 2:
            return {"tool": "exec", "args": args}
        assert "status 0" in observations[-1], observations
        return {"tool": "finish", "args": ["The retry passed after creating the file."]}
    if task == "Open Python with auto-approval enabled":
        if not observations:
            return {"tool": "terminal", "args": ["python"]}
        assert "declined" in observations[0], observations
        return {"tool": "finish", "args": ["Terminal declined."]}
    if task == "Check that command approval is back":
        assert "Auto-approval is enabled" not in instruction
        if not observations:
            return {"tool": "exec", "args": ["touch", "declined.txt"]}
        assert "declined" in observations[0], observations
        return {"tool": "finish", "args": ["Command declined."]}
    assert "I found note.txt." in data["conversation"]
    if task == "Check it again":
        if not observations:
            return {"tool": "exec", "args": ["cat", "note.txt"]}
        assert "hello-chat" in observations[-1]
        return {"tool": "finish", "args": ["Still present."]}
    if task == "Can you delete it?":
        if not observations:
            return {"tool": "exec", "args": ["rm", "note.txt"]}
        assert "declined" in observations[-1]
        return {"tool": "finish", "args": ["Deletion declined."]}
    if task == "Run my tool":
        if not observations:
            return {"tool": "exec", "args": ["./demo", "one", "two words", "three", "four"]}
        if len(records) == 1:
            assert "status 23" in observations[-1] and "argc=5" in observations[-1], observations
            return {"tool": "verify", "args": ["demo", './demo one "two words" three four']}
        assert records[-1]["artifact"]["run"]["exit_code"] == 23, records
        return {"tool": "finish", "args": ["The custom tool ran."]}
    if task == "Let me use Python":
        if not observations:
            return {"tool": "terminal", "args": ["python"]}
        assert "status 0" in observations[-1], observations
        return {"tool": "finish", "args": ["Terminal returned."]}
    if task == "Compile a second tool":
        if not observations:
            return {"tool": "exec", "args": ["tcc", "demo.c", "-o", "demo2"]}
        assert "status 0" in observations[-1], observations
        return {"tool": "finish", "args": ["Compiled the tool."]}
    if task == "Show Git status":
        if not observations:
            return {"tool": "exec", "args": ["git", "status"]}
        assert "note.txt" in observations[-1] and "status 0" in observations[-1], observations
        return {"tool": "finish", "args": ["Git sees note.txt."]}
    if task == "Run the spinning tool":
        assert not observations, observations
        return {"tool": "exec", "args": ["./spin"]}
    raise bridge.BridgeError("unexpected chat fixture task")


def test():
    with tempfile.TemporaryDirectory() as tmp:
        disk = Path(tmp) / "chat.img"
        with serial.boot(disk) as guest:
            guest.expect(b"mkdir work\r", serial.SHELL)
            guest.expect(b"echo hello-chat > work/note.txt\r", serial.SHELL)
            for line in ('#include <stdio.h>',
                         'int main(int argc,char **argv){ printf("argc=%d %s\\n",argc,argv[2]);',
                         'for(volatile unsigned i=0;i<600000000;i++){}',
                         'puts("command done"); return 23;}'):
                guest.expect(f"echo '{line}' >> work/demo.c\r".encode(), serial.SHELL)
            guest.expect(b"tcc work/demo.c -o work/demo\r", serial.SHELL)
            guest.expect(b"echo '#include <stdio.h>' > work/spin.c\r", serial.SHELL)
            guest.expect(b"""echo 'int main(void){puts("running spin"); for(;;){} }' >> work/spin.c\r""", serial.SHELL)
            guest.expect(b"tcc work/spin.c -o work/spin\r", serial.SHELL)
            guest.expect(b"cd work\r", b"lixin:/work$ ")
            guest.expect(b"git init\r", b"lixin:/work$ ")
            guest.send(b"shutdown\r")
            assert guest.vm.wait(timeout=10) == 0

        qemu = [
            "qemu-system-aarch64", "-machine", "virt", "-accel", "hvf", "-cpu", "host",
            "-smp", "1", "-m", "512M", "-display", "none", "-serial", "stdio", "-monitor", "none",
            "-global", "virtio-mmio.force-legacy=false",
            "-drive", f"if=none,id=fs,format=raw,cache=writethrough,file={disk}",
            "-device", "virtio-blk-device,drive=fs,bus=virtio-mmio-bus.0",
            "-object", "rng-random,id=entropy,filename=/dev/urandom",
            "-device", "virtio-rng-device,rng=entropy,bus=virtio-mmio-bus.5",
            "-netdev", "user,id=outbound",
            "-device", "virtio-net-device,netdev=outbound,bus=virtio-mmio-bus.4",
            "-kernel", str(serial.ROOT / "Kernel/target/aarch64-unknown-none-softfloat/release/lixin-kernel"),
        ]
        with serial.boot(disk, [sys.executable, __file__, "--fixture", *qemu]) as guest:
            guest.send(b"agent chat /work\r")
            output = guest.until(PROMPT)
            assert b"LixinOS Agent" in output and b"Bypass on" in output and b"/work" in output, output
            assert b"50 tools/turn" in output and b"/help" in output, output
            guest.expect(b"\x1b[A/prompt\r", PROMPT, b"For CPU questions, run cpuinfo once.")
            guest.expect(b"/bypass\r", PROMPT, b"Bypass off")
            guest.expect(b"/export\r", PROMPT, b"no conversation to export yet")
            guest.send(b"Hello\r")
            output = guest.until(PROMPT)
            assert REPLY_COLOR + b"Hello!" + COLOR_RESET in output and b"[01/" not in output, output
            assert b"Thinking" in output and output.count(b"\x1b[2K") >= 2, output
            guest.send(b"Please list the workspace\r")
            output = guest.until(PROMPT)
            heading = b'exec in "/work": "/bin/ls" "-1" "."'
            assert COMMAND_COLOR + heading + COLOR_RESET + b"\n" in output, output
            assert output.count(b"[01/50]") == 1, output
            assert REPLY_COLOR + b"I found note.txt." + COLOR_RESET + b"\n" in output, output
            assert b"command exited with status 0" not in output, output
            guest.send(b"Check it agai\x1b[A")
            guest.until(PROMPT + b"Please list the workspace")
            guest.send(b"\x1bOA")
            guest.until(PROMPT + b"Hello")
            guest.send(b"\x1b[B")
            guest.until(PROMPT + b"Please list the workspace")
            guest.send(b"\x1bOB")
            guest.until(PROMPT + b"Check it agai")
            guest.expect(b"n\r", PROMPT, b"Still present.")
            guest.expect(b"/export\r", PROMPT, b"exported conversation to /work/agent-conversation.txt")
            guest.expect(b"/export\r", PROMPT, b"exported conversation to /work/agent-conversation-2.txt")
            guest.expect(b"/export ../escaped.txt\r", PROMPT, b"use a filename in the project directory")
            guest.expect(b"/export note.txt\r", PROMPT, b"export file already exists")
            guest.send(b"\x1b[A")
            guest.until(PROMPT + b"/export note.txt")
            guest.expect(b"\x7f" * len("note.txt") + b"recalled.txt\r", PROMPT,
                         b"exported conversation to /work/recalled.txt")
            guest.expect(b"Find the file containing hello-chat\r", APPROVAL, b'exec in "/work": "/bin/find"')
            guest.expect(b"y\r", APPROVAL, b'exec in "/work": "/bin/grep"')
            guest.expect(b"y\r", PROMPT, b"The match is note.txt line 1.")
            guest.expect(b"Can you delete it?\r", APPROVAL, b'"/bin/rm" "note.txt"')
            guest.expect(b"n\r", PROMPT, b"Deletion declined.")
            guest.expect(b"Run my tool\r", APPROVAL, b'"/work/demo" "one" "two words" "three" "four"')
            guest.send(b"y\r")
            guest.until(b"argc=5 two words\r\n")
            assert b"agent: command exited" not in guest.buffer, "output arrived only after the command exited"
            guest.until(b"command done\r\n")
            assert b"status 23" in guest.until(PROMPT)
            guest.expect(b"Compile a second tool\r", APPROVAL)
            guest.expect(b"y\r", PROMPT, b"Compiled the tool.")
            guest.expect(b"Show Git status\r", APPROVAL)
            guest.expect(b"y\r", PROMPT, b"Git sees note.txt.")
            guest.expect(b"Run the spinning tool\r", APPROVAL)
            guest.expect(b"y\r", b"running spin\r\n")
            guest.expect(b"\x07", PROMPT, b"turn cancelled")
            guest.expect(b"Let me use Python\r", APPROVAL)
            guest.expect(b"y\r", serial.PYTHON)
            guest.expect(b"print(42)\r", serial.PYTHON, b"\n42\n")
            guest.expect(b"\x04", PROMPT, b"Terminal returned.")
            review = b"Apply the staged scripts to disk? [y/N] "
            guest.send(b"Stage a file and keep it\r")
            output = guest.until(review)
            assert output.count(PREVIEW) == 2 and b"# beyond the preview" not in output, output
            guest.expect(b"v", review, STAGED_CODE.encode())
            guest.expect(b"y", PROMPT, b"applied staged scripts")
            guest.send(b"Read the staged script from disk\r")
            output = guest.until(PROMPT)
            assert PREVIEW in output.replace(COLOR_RESET, b"") and b"# beyond the preview" not in output, output
            assert b"Read the full file." in output and b"command exited with status 0" not in output, output
            guest.expect(b"Stage a file and discard it\r", review)
            guest.expect(b"n", PROMPT, b"discarded staged scripts")
            guest.expect(b"Run a shell pipeline\r", APPROVAL, b'"/bin/bash" "-c"')
            guest.expect(b"y\r", PROMPT, b"The shell pipeline finished.")
            guest.send(b"Create 100 random number files\r")
            output = guest.until(APPROVAL)
            assert b'"/bin/python" "-c"' in output and b"for i in range(100):" in output, output
            assert b"with open" not in output, output
            guest.expect(b"v\r", APPROVAL, b"with open")
            guest.send(b"y\r")
            output = guest.until(PROMPT, timeout=65)
            assert b"Created 100 random number files." in output, output
            guest.expect(b"Verify staged tests cannot write files\r", review, b"test passed")
            guest.expect(b"n", PROMPT, b"discarded staged scripts")
            guest.send(b"Build and test Snake\r")
            output = guest.until(review)
            assert b"must pass its test before review" in output and b"Snake is ready" not in output, output
            guest.send(b"y")
            output = guest.until(PROMPT)
            assert b"Verified" + COLOR_RESET + b'  "/work/snake.py"' in output and b"Snake is ready" in output, output
            assert b"1 exact passed, 1 smoke passed, 0 failed" in output, output
            assert b"Scripted input verified." in output and b"Run" + COLOR_RESET + b"       python /work/snake.py" in output, output
            guest.send(b"Check test timeout status\r")
            output = guest.until(PROMPT)
            assert b"test timed out" in output and b"must pass its test before review" in output, output
            assert b"The timed-out candidate was saved" not in output and review not in output, output
            guest.expect(b"How is my CPU?\r", PROMPT, b"Measured guest CPU usage was ")
            guest.expect(b"Read CPU counters from proc\r", PROMPT, b"Read live counters from /proc/stat.")
            guest.expect(b"Check identity before a model error\r", PROMPT, b"DeepSeek HTTP 401")
            guest.expect(b"What did you learn before that error?\r", PROMPT, b"The guest is LixinOS.")
            guest.expect(b"Recover from a malformed reply\r", PROMPT, b"Recovered without running uname again.")
            guest.expect(b"Read the shipped build guide\r", PROMPT, b"The build guide was available.")
            guest.expect(b"/export session.txt\r", PROMPT, b"exported conversation to /work/session.txt")
            guest.expect(b"/bypass\r", PROMPT, b"Bypass on")
            guest.send(b"Create a marker automatically\r")
            output = guest.until(PROMPT)
            assert APPROVAL not in output and b"command exited with status 0" not in output, output
            assert b"\nmarker output" + COLOR_RESET + b"\n" in output, output
            assert REPLY_COLOR + b"Marker created." in output, output
            guest.send(b"Modify a verified file\r")
            output = guest.until(PROMPT)
            assert b"artifact changed after verification" in output and b"Verified the updated file." in output, output
            assert b"stale receipt should never" not in output, output
            guest.expect(b"Open Python with auto-approval enabled\r", APPROVAL)
            guest.expect(b"n\r", PROMPT, b"Terminal declined.")
            key = Path(tmp) / "ssh-host-key"
            subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", key], check=True)
            ssh = load("ssh_test", "test-ssh-out.py")
            with ssh.fixture(key, 0, "agent-ssh-fixture") as port:
                guest.send(f"Open local SSH {port}\r".encode())
                output = guest.until(APPROVAL)
                assert output.count(b"SSH sessions require the agent's terminal tool") == 2, output
                assert b"Hand over the terminal" in output and b"terminal handoff" in output, output
                guest.expect(b"y\r", b"Trust this host (yes/no)? ")
                guest.expect(b"yes\r", b"password: ")
                guest.expect(b"agent-ssh-fixture\r", b"remote$ ")
                guest.expect(b"exit\r", PROMPT, b"SSH terminal closed successfully.")
            guest.send(b"Repeat the failed command\r")
            output = guest.until(PROMPT)
            assert output.count(b"command exited with status 1") == 1, output
            assert output.count(b"skipped repeated exec") == 2 and b"stopped after repeated failed commands" in output, output
            guest.expect(b"Retry after changing the workspace\r", PROMPT, b"The retry passed after creating the file.")
            guest.expect(b"Stage a file and discard it\r", review)
            guest.expect(b"n", PROMPT, b"discarded staged scripts")
            guest.expect(b"/bypass\r", PROMPT, b"Bypass off")
            guest.expect(b"Check that command approval is back\r", APPROVAL, b'"declined.txt"')
            guest.expect(b"n\r", PROMPT, b"Command declined.")
            guest.expect(b"/export automatic.txt\r", PROMPT, b"/work/automatic.txt")
            guest.expect(b"/exit\r", serial.SHELL)
            guest.expect(b"agent chat --auto-approve /work\r", PROMPT, b"Bypass on")
            guest.expect(b"/exit\r", serial.SHELL)
            guest.expect(b"agent chat /work\r", PROMPT, b"Bypass on")
            guest.expect(b"/help\r", PROMPT, b"Recall inputs")
            guest.send(b"Create a marker automatically\r")
            output = guest.until(PROMPT)
            assert b"Marker created." in output and APPROVAL not in output, output
            guest.expect(b"/exit\r", serial.SHELL)
            guest.expect(b"cat work/note.txt\r", serial.SHELL, b"hello-chat")
            guest.send(b"shutdown\r")
            assert guest.vm.wait(timeout=10) == 0
        with serial.boot(disk) as guest:
            guest.expect(b"python /work/snake.py\r", b"Q to quit.")
            guest.expect(b"\x1b[A", b"Arrows or WASD to move")
            guest.expect(b"q", serial.SHELL, b"\nBye.\n")
            guest.send(b"cat /work/session.txt\r")
            transcript = guest.until(serial.SHELL)
            assert COMMAND_COLOR not in transcript and REPLY_COLOR not in transcript, transcript
            for marker in (
                b"User: Please list the workspace", b"Agent: I found note.txt.",
                b'Tool exec ["ls", "-1", "."]', b"Command exited with status 23.",
                b"Result: turn cancelled. Discarded staged scripts.", b"Agent: Terminal returned.",
                b"Review: applied staged scripts", b"Review: discarded staged scripts",
                b'Verified "/work/snake.py" on disk',
                b'Tool exec ["cpuinfo"]', b"guest_busy_percent=",
                b"Model error: DeepSeek HTTP 401",
                b"Model reply retried: invalid model project response",
            ):
                assert marker in transcript, transcript
            guest.expect(b"cat /escaped.txt\r", serial.SHELL, b"file not found")
            guest.expect(b"cat /work/escaped.txt\r", serial.SHELL, b"file not found")
            guest.expect(b"cat /work/shell.txt\r", serial.SHELL, b"\npipeline\n")
            guest.expect(b"cat /work/kept.py\r", serial.SHELL, STAGED_CODE.encode())
            guest.expect(b"stat /work/auto-ok.txt\r", serial.SHELL, b"Type: regular file")
            guest.expect(b"cat /work/declined.txt\r", serial.SHELL, b"file not found")
            guest.send(b"cat /work/automatic.txt\r")
            output = guest.until(serial.SHELL)
            assert b"Approval: auto-approved guest command" in output, output
            assert b"agent-ssh-fixture" not in output and b"remote$" not in output, output
            guest.expect(b"ls -1 /work | grep num_ | wc -l\r", serial.SHELL, b"\n100\n")
            guest.send(b"shutdown\r")
            assert guest.vm.wait(timeout=10) == 0


if __name__ == "__main__":
    if sys.argv[1:2] == ["--fixture"]:
        sys.exit(bridge.launch(sys.argv[2:], reply))
    test()
    print("Verified chat context, live guest output, approvals, terminal handoff and export across reboot.")
