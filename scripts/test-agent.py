"""Exercise the real QEMU bridge and guest Agent with deterministic model responses."""
import importlib.util
from functools import partial
from io import BytesIO
import json
from pathlib import Path
import re
import socket
import sys
import tempfile
from threading import Thread
import time
import urllib.error


def load_script(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


bridge = load_script("agent_bridge", "agent-bridge.py")
serial = load_script("test_user", "test-user.py")

TASK = "double a number read from stdin"
TESTS = [{"stdin": "21\n", "stdout": "42\n"}, {"stdin": "-3\n", "stdout": "-6\n"}]
WRONG = "# " + "x" * 5000 + "\nprint(int(input()) + 2)\n"
LOOP = "while True:\n    pass\n"
RIGHT = "print(int(input()) * 2)\n"
IMPROVE = "Print invalid for non-integer input while keeping the existing behavior"
FAIL_IMPROVE = "Always return an incorrect candidate"
LEGACY = "Default to 0 for empty input and still triple integers"
NEW_CASES = [{"stdin": "oops\n", "stdout": "invalid\n"}]
IMPROVED = "try:\n    print(int(input()) * 2)\nexcept ValueError:\n    print('invalid')\n"
LEGACY_SOURCE = "print(int(input()) * 3)\n"
LEGACY_TESTS = [{"stdin": "7\n", "stdout": "21\n"}]
LEGACY_EXAMPLES = [{"stdin": "\n", "stdout": "0\n"}]
INSPECT = "Report CPU, RAM, disk, filesystem, and the file in the current directory"
UNSUPPORTED = "Try to remove sample.txt, then check whether it still exists"
INSPECT_IDENTITY = "Is this a Linux system and what is its name?"
GIT_REFERENCE_MARKER = "Guest Git reference loaded from disk."
FILE_CONTENT = b"guest-private-content"
PROJECT = "Stage two scripts and preserve the workspace boundary"
PROJECT_CHECK = "Check the persisted project with the rebuilt harness"
PROJECT_INTERACT = "Let the operator redirect this project task"
PROJECT_CANCEL = "Start a test the operator can cancel"
PROJECT_BUDGET = "Use the full tool budget and finish"
BUDGET_SOURCE = "# " + "x" * 4000 + "\nprint('ready')\n"
PROJECT_TUNE = "Use a budget of 3 tools and show 64 bytes"
PROJECT_TUNED_LIMIT = "Try to exceed the tuned budget"
SYSTEM_UPDATE = "Stage a compiled executable and edit files in two directories"


def mock_inspect(instruction, data, directory_count):
    assert "LixinOS read-only system inspector" in instruction
    assert "does not run a Linux kernel" in " ".join(instruction.split())
    assert "git diff --staged" in data["git_reference"]
    assert data["cwd"] == "/lab"
    observations = data["observations"]
    assert data["remaining_queries"] == 6 - len(observations)
    assert all(FILE_CONTENT.decode() not in item for item in observations)
    if data["question"] == INSPECT_IDENTITY:
        if not observations:
            return {"queries": [{"tool": "system"}]}
        assert "name=LixinOS" in observations[0]
        assert "linux_kernel=false" in observations[0]
        assert "architecture=aarch64" in observations[0]
        return {"answer": "The guest is LixinOS, a standalone ARM64 OS."}
    if data["question"] == UNSUPPORTED:
        if len(observations) == 0:
            return {"queries": [{"tool": "rm", "path": "sample.txt"}]}
        assert "unsupported inspection tool" in observations[0]
        if len(observations) == 1:
            return {"queries": [{"tool": "stat", "path": "sample.txt"}]}
        assert 'path="/lab/sample.txt"' in observations[1]
        return {"answer": "Removal is unavailable. sample.txt still exists."}

    assert data["question"] == INSPECT
    tools = [
        ({"tool": "cpu"}, ("vcpus=1", "uptime_seconds=", "guest_busy_percent=", "sample_ms=")),
        ({"tool": "memory"}, ("guest_ram_bytes=167772160", "heap_used_bytes=", "heap_free_bytes=")),
        ({"tool": "disk"}, ("capacity_bytes=143654912", "committed_snapshot_generation=")),
        ({"tool": "filesystem"}, ("files=", f"directories={directory_count} (includes root)", "file_content_bytes=")),
        ({"tool": "list", "path": "."}, ('path="/lab"', 'file "sample.txt" (22 bytes)')),
        ({"tool": "stat", "path": "sample.txt"}, ('path="/lab/sample.txt"', "size_bytes=22")),
    ]
    for observation, (tool, evidence) in zip(observations, tools):
        assert f'tool="{tool["tool"]}"' in observation, observation
        assert all(value in observation for value in evidence), observation
        if tool["tool"] == "cpu":
            busy = re.search(r"guest_busy_percent=(\d+)", observation)
            sample = re.search(r"sample_ms=(\d+)", observation)
            assert busy and 0 <= int(busy[1]) <= 100, observation
            assert sample and int(sample[1]) >= 250, observation
    if not observations:
        return {"queries": [tool for tool, _ in tools[:4]]}
    if len(observations) == 4:
        return {"queries": [tool for tool, _ in tools[4:]]}
    return {"answer": "1 vCPU, 160 MiB of RAM, 137 MiB disk, and sample.txt is 22 bytes.\x1b[2J"}


def mock_deepseek(request, timeout, *, directory_count):
    assert request.full_url == bridge.ENDPOINT
    assert request.get_header("Authorization") == "Bearer fixture-key"
    payload = json.loads(request.data)
    assert timeout == 45 and payload["response_format"] == {"type": "json_object"}
    instruction, content = payload["messages"]
    data = json.loads(content["content"])
    if "question" in data:
        result = mock_inspect(instruction["content"], data, directory_count)
    elif "workspace" in data:
        result = mock_project(instruction["content"], data)
    elif bridge.BASELINE_PROMPT in instruction["content"]:
        assert data == {"active_script": LEGACY_SOURCE}, data
        result = {"tests": LEGACY_TESTS}
    elif bridge.EXAMPLES_PROMPT in instruction["content"]:
        assert data == {"task": LEGACY, "active_script": LEGACY_SOURCE}, data
        result = {"tests": LEGACY_EXAMPLES}
    elif data["task"] == "reject with HTTP error":
        raise urllib.error.HTTPError(request.full_url, 401, "Fixture error", {}, BytesIO())
    elif bridge.TEST_PROMPT in instruction["content"]:
        result = {"tests": TESTS}
    elif data["task"] in (IMPROVE, FAIL_IMPROVE, LEGACY):
        result = mock_improvement(data)
    else:
        assert data["tests"] == TESTS
        previous = data["previous_script"]
        feedback = data["failure"]
        if data["task"] == "always flood":
            if previous:
                assert "test exceeded the output limit" in feedback
            result = {"script": "while True:\n    print('0123456789')\n"}
        else:
            assert data["task"] == TASK
            if not previous:
                assert not feedback
                script = WRONG
            elif previous == WRONG:
                assert 'expected "42\\n", got "23\\n"' in feedback, feedback
                script = LOOP
            else:
                assert previous == LOOP and "test timed out" in feedback, data
                script = RIGHT
            result = {"script": script}
    return BytesIO(json.dumps({
        "choices": [{"finish_reason": "stop", "message": {"content": json.dumps(result)}}]
    }).encode())


def mock_improvement(data):
    if data["task"] == LEGACY:
        assert data["active_script"] == LEGACY_SOURCE
        assert data["tests"] == LEGACY_TESTS + LEGACY_EXAMPLES
        return {"script": "print(int(input() or '0') * 3)\n"}
    assert data["tests"] == TESTS + NEW_CASES, data
    if data["task"] == FAIL_IMPROVE:
        assert data["active_script"] == IMPROVED
        return {"script": "print('invalid')\n"}
    assert data["active_script"] == RIGHT
    if not data["previous_script"]:
        # This passes the new case and breaks the original behavior.
        return {"script": "print('invalid')\n"}
    assert 'expected "42\\n", got "invalid\\n"' in data["failure"], data
    return {"script": IMPROVED}


def mock_project(instruction, data):
    assert "LixinOS project agent" in instruction
    assert "does not run a Linux kernel" in " ".join(instruction.split())
    assert "git diff --staged" in data["git_reference"]
    observations = data["observations"]
    if data["task"] == SYSTEM_UPDATE:
        assert data["workspace"] == "/" and "system mode" in instruction
        actions = [
            ("system", [], "linux_kernel=false"),
            ("list", ["/proc"], 'file "stat"'),
            ("read", ["/proc/stat"], "cpu0 "),
            ("remove", ["/proc/stat"], "read-only filesystem"),
            ("read", ["/work/preview.txt"], "p" * 96),
            ("replace", ["/work/preview.txt", "p" * 96, "changed"], "staged replace"),
            ("remove", ["/work/note"], "staged remove"),
            ("mkdir", ["/system-check"], "staged mkdir"),
            ("writehex", ["/system-check/data", "0061ff"], "staged writehex"),
            ("readhex", ["/system-check/data", "0"], "0061ff"),
            ("write", ["/system-check/hello.c", '#include <stdio.h>\nint main(void) { puts("Guest build"); return 0; }\n'], "staged write"),
            ("compile", ["/bin/agent-demo", "/system-check/hello.c"], "using compile"),
            ("finish", ["System batch is ready for review."], ""),
        ]
        for observation, (tool, _, evidence) in zip(observations, actions):
            assert observation.startswith(f'tool "{tool}"') and evidence in observation, observation
        tool, args, _ = actions[len(observations)]
        return {"tool": tool, "args": args}
    if data["task"] == PROJECT_TUNE:
        assert data["workspace"] == "/agent-policy"
        assert data["remaining_steps"] == 12 - len(observations)
        actions = [
            ("read", ["settings.py"]),
            ("write", ["settings.py", "print('arbitrary code')\n"]),
            ("write", ["settings.py", 'print("3 64")\n']),
            ("test", ["settings.py", "", "3 64\n"]),
            ("finish", ["Settings ready."]),
        ]
        if len(observations) == 1:
            assert 'print("50 512")' in observations[0]
        if len(observations) == 2:
            assert "stage only settings.py" in observations[1]
        if len(observations) == 4:
            assert "test passed" in observations[3]
        tool, args = actions[len(observations)]
        return {"tool": tool, "args": args}

    assert data["workspace"] == "/work"
    if data["task"] == PROJECT_TUNED_LIMIT:
        assert data["remaining_steps"] == 3 - len(observations)
        if not observations:
            return {"tool": "read", "args": ["preview.txt"]}
        assert "p" * 96 in observations[0], observations[0]
        return {"tool": "list", "args": ["."]}
    assert data["remaining_steps"] == 50 - len(observations)
    if data["task"] == PROJECT_BUDGET:
        if not observations:
            return {"tool": "write", "args": ["budget.py", BUDGET_SOURCE]}
        if len(observations) == 1:
            return {"tool": "test", "args": ["budget.py", "", "ready\n"]}
        if data["remaining_steps"]:
            return {"tool": "read", "args": ["budget.py"]}
        assert all(BUDGET_SOURCE in observation for observation in observations[2:])
        return {"tool": "finish", "args": ["Finished after 50 tool calls."]}
    if data["task"] == PROJECT_CANCEL:
        if not observations:
            return {"tool": "write", "args": ["spin.py", "while True:\n    pass\n"]}
        return {"tool": "test", "args": ["spin.py", "", ""]}
    if data["task"] == PROJECT_INTERACT:
        updates = data["operator_updates"]
        if not updates:
            time.sleep(0.5)
            return {"tool": "write", "args": ["stale.py", "print('stale')\n"]}
        assert updates.startswith("Print changed.\n"), data
        if not observations:
            return {"tool": "write", "args": ["updated.py", "print('changed')\n"]}
        if len(observations) == 1:
            return {"tool": "test", "args": ["updated.py", "", "changed\n"]}
        assert "test passed" in observations[-1], data
        if "Keep the staged script.\n" in updates:
            return {"tool": "finish", "args": ["Follow-up at review accepted."]}
        return {"tool": "finish", "args": ["Ready for the first review."]}
    if data["task"] == PROJECT_CHECK:
        assert GIT_REFERENCE_MARKER in data["git_reference"]
        if not observations:
            return {"tool": "read", "args": ["double.py"]}
        assert "print(int(input()) * 2)" in observations[0]
        return {"tool": "finish", "args": ["The rebuilt harness read the saved script."]}

    assert data["task"] == PROJECT
    actions = [
        ("read", ["../lab/sample.txt"], "path outside the allowed project files"),
        ("list", ["."], "file \"note\""),
        ("write", ["double.py", "print(int(input()) + 1)\n"], "staged"),
        ("test", ["double.py", "21\n", "42\n"], "test failed"),
        ("write", ["double.py", RIGHT], "staged"),
        ("test", ["double.py", "21\n", "42\n"], "test passed"),
        ("write", ["message.py", "print('ready')\n"], "staged"),
        ("test", ["message.py", "", "ready\n"], "test passed"),
        ("finish", ["Both staged scripts passed their tests."], ""),
    ]
    for observation, (tool, _, evidence) in zip(observations, actions):
        assert observation.startswith(f'tool "{tool}"') and evidence in observation, observation
    tool, args, _ = actions[len(observations)]
    return {"tool": tool, "args": args}


def check_chunked_bridge():
    calls = []

    def complete(instruction, data):
        calls.append((instruction, data["question"]))
        return {"answer": "ready"}

    def chunk(request_id, offset, total, data):
        return b"@%d\t%d\t%d\t%s\n" % (request_id, offset, total, data)

    client, server = socket.socketpair()
    with client, server, client.makefile("rb") as incoming:
        client.settimeout(5)
        relay = Thread(target=bridge.serve, args=(server, complete))
        relay.start()
        try:
            size = bridge.REQUEST_CHUNK
            client.sendall(chunk(1, 0, size + 5, b"x" * size))
            assert incoming.readline() == b"1\tack\n"
            # A new request can replace a partially sent request after Ctrl+P.
            instruction = "x" * size
            fields = [instruction, "check", "/", "6"]
            request = b"2\tinspect\t" + b"\t".join(field.encode().hex().encode() for field in fields)
            for offset in range(0, len(request), size):
                client.sendall(chunk(2, offset, len(request), request[offset:offset + size]))
                if offset + size < len(request):
                    assert incoming.readline() == b"2\tack\n"
            assert incoming.readline() == bridge.encode_response(b"2", b"ok", ["answer", "ready"])
            assert calls == [(instruction, "check")]
            client.sendall(chunk(3, 0, 10, b"short"))
            assert incoming.readline() == bridge.encode_response(b"3", b"error", ["invalid request chunk"])
        finally:
            client.shutdown(socket.SHUT_WR)
            relay.join(timeout=5)
            assert not relay.is_alive()


def test():
    with tempfile.TemporaryDirectory() as temporary:
        disk = Path(temporary) / "agent.img"
        with serial.boot(disk) as s:
            s.expect(b"uname -s | cat\r", serial.SHELL, b"\nLixinOS\n")
            s.expect(b"uname -m\r", serial.SHELL, b"\naarch64\n")
            s.expect(b"uname -a\r", serial.SHELL, b"(standalone; non-Linux)")
            s.expect(b"sysinfo\r", serial.SHELL, b"linux_kernel=false")
            s.expect(b'agent inspect "How much RAM?"\r', serial.SHELL, b"model bridge unavailable")
            s.expect(b'agent offline.py "demo"\r', serial.SHELL, b"model bridge unavailable")
            s.expect(b"cat offline.py\r", serial.SHELL, b"file not found")
            s.send(b"find / -type d\r")
            directories = s.until(serial.SHELL).splitlines()[1:]
            directory_count = sum(path != b"/proc" for path in directories)
        with disk.open("r+b") as image:
            image.truncate(137 * 1024 * 1024)

        qemu = [
            "qemu-system-aarch64", "-machine", "virt", "-accel", "hvf", "-cpu", "host",
            "-smp", "1", "-m", "160M", "-display", "none", "-serial", "stdio", "-monitor", "none",
            "-global", "virtio-mmio.force-legacy=false",
            "-drive", f"if=none,id=fs,format=raw,cache=writethrough,file={disk}",
            "-device", "virtio-blk-device,drive=fs,bus=virtio-mmio-bus.0",
            "-kernel", str(serial.ROOT / "Kernel/target/aarch64-unknown-none-softfloat/release/lixin-kernel"),
        ]
        # The inspection fixture creates one directory, /lab.
        command = [sys.executable, __file__, "--model-fixture", str(directory_count + 1), *qemu]
        with serial.boot(disk, command) as s:
            s.expect(b"mkdir lab\r", serial.SHELL)
            s.expect(b"echo " + FILE_CONTENT + b" > lab/sample.txt\r", serial.SHELL)
            s.expect(b"cd lab\r", b"lixin:/lab$ ")
            disk_before = disk.read_bytes()
            s.send(f'agent inspect "{INSPECT}"\r'.encode())
            output = s.until(b"lixin:/lab$ ")
            assert b"1 vCPU, 160 MiB of RAM, 137 MiB disk, and sample.txt is 22 bytes." in output, output
            assert b"\x1b[2J" not in output, output
            s.expect(f'agent inspect "{UNSUPPORTED}"\r'.encode(), b"lixin:/lab$ ", b"sample.txt still exists")
            s.expect(f'agent inspect "{INSPECT_IDENTITY}"\r'.encode(),
                     b"lixin:/lab$ ", b"The guest is LixinOS, a standalone ARM64 OS.")
            assert disk.read_bytes() == disk_before, "inspection modified the guest disk"
            s.expect(b"cd /\r", serial.SHELL)
            s.expect(b'agent auth.py "reject with HTTP error"\r', serial.SHELL, b"DeepSeek HTTP 401")
            s.expect(b'agent broken.py "always flood"\r', serial.SHELL, b"exhausted its attempts")
            s.expect(b"cat broken.py\r", serial.SHELL, b"file not found")
            s.send(f'agent double.py "{TASK}"\r'.encode())
            output = s.until(serial.SHELL, timeout=20)
            for marker in (
                b"generating script (1/3)", b"expected",
                b"generating script (2/3)", b"test timed out",
                b"generating script (3/3)", b"all 2 tests passed", b"saved /double.py",
            ):
                assert marker in output, output
            s.expect(b'agent double.py "replace it"\r', serial.SHELL, b"output already exists")
            s.send(b"shutdown\r")
            s.until(b"Shutting down LixinOS")
            assert s.vm.wait(timeout=10) == 0

        with serial.boot(disk) as s:
            s.expect(b"cat double.py\r", serial.SHELL, RIGHT.encode())
            s.expect(b"python double.py\r17\r", serial.SHELL, b"\n34\n")

        with serial.boot(disk, command) as s:
            examples = json.dumps(NEW_CASES).encode()
            s.expect(b"echo '" + examples + b"' > new.json\r", serial.SHELL)
            s.send(f'agent improve double.py "{IMPROVE}" --examples new.json\r'.encode())
            result = s.until(serial.SHELL)
            for marker in (b"passed 2 baseline tests", b"expected", b"all 3 tests passed", b"activated /double.py"):
                assert marker in result, result
            s.expect(b"cat double.py\r", serial.SHELL, IMPROVED.encode())
            s.expect(f'agent improve double.py "{FAIL_IMPROVE}" --examples new.json\r'.encode(),
                     serial.SHELL, b"exhausted its attempts")
            s.expect(b"cat double.py\r", serial.SHELL, IMPROVED.encode())

            s.expect(b"echo '" + LEGACY_SOURCE.rstrip().encode() + b"' > legacy.py\r", serial.SHELL)
            s.send(f'agent improve legacy.py "{LEGACY}"\r'.encode())
            result = s.until(serial.SHELL)
            for marker in (b"no saved tests", b"passed 1 baseline tests", b"all 2 tests passed", b"activated /legacy.py"):
                assert marker in result, result
            s.send(b"shutdown\r")
            s.until(b"Shutting down LixinOS")
            assert s.vm.wait(timeout=10) == 0

        with serial.boot(disk) as s:
            s.expect(b"python double.py\roops\r", serial.SHELL, b"\ninvalid\n")
            s.expect(b"python double.py\r17\r", serial.SHELL, b"\n34\n")
            s.expect(b"agent rollback double.py\r", serial.SHELL, b"restored previous version")
            s.expect(b"cat double.py\r", serial.SHELL, RIGHT.encode())
            s.expect(b"agent rollback double.py\r", serial.SHELL, b"no previous version")
            s.expect(b"python legacy.py\r\r", serial.SHELL, b"\n0\n")
            s.expect(b"agent rollback legacy.py\r", serial.SHELL, b"restored previous version")

        with serial.boot(disk) as s:
            s.expect(b"cat double.py\r", serial.SHELL, RIGHT.encode())
            s.expect(b"cat legacy.py\r", serial.SHELL, LEGACY_SOURCE.encode())

        with serial.boot(disk, command) as s:
            s.expect(b"mkdir work\r", serial.SHELL)
            s.expect(b"touch work/note\r", serial.SHELL)
            s.expect(b"/bin/agent-run\r", serial.SHELL, b"[exit 1]")
            s.send(f'agent run /work "{PROJECT_INTERACT}"\r'.encode())
            s.until(b"Ctrl+G cancels.")
            time.sleep(0.12)
            s.send(b"\x10")
            s.until(b"agent> ")
            s.send(b"Print changed.\r")
            confirm = b"Apply the staged scripts to disk? [y/N] "
            output = s.until(confirm)
            assert b"Ready for the first review." in output, output
            s.send(b"\x10")
            s.until(b"agent> ")
            s.send(b"Keep the staged script.\r")
            output = s.until(confirm)
            assert b"Follow-up at review accepted." in output, output
            s.expect(b"y", serial.SHELL, b"applied staged scripts")
            s.expect(b"python work/updated.py\r", serial.SHELL, b"\nchanged\n")
            s.expect(b"cat work/stale.py\r", serial.SHELL, b"file not found")

            s.send(f'agent run /work "{PROJECT_CANCEL}"\r'.encode())
            s.until(b'agent: [02/50] test "spin.py"\r\n')
            s.expect(b"\x07", serial.SHELL, b"cancelled. Discarded staged scripts")
            s.expect(b"cat work/spin.py\r", serial.SHELL, b"file not found")
            operation = f'agent run /work "{PROJECT}"\r'.encode()
            for answer in (b"n", b"y"):
                s.send(operation)
                output = s.until(b"Apply the staged scripts to disk? [y/N] ")
                assert b"starting /bin/agent-run" in output and b"test failed" in output, output
                assert b'agent: [02/50] list "."\nfile "note"\n' in output, output
                assert b'agent: [04/50] test "double.py"\ntest failed' in output, output
                assert b'+++ proposed "/work/double.py"' in output, output
                s.send(answer)
                s.until(serial.SHELL)
                if answer == b"n":
                    s.expect(b"cat work/double.py\r", serial.SHELL, b"file not found")
                    s.expect(b"cat work/message.py\r", serial.SHELL, b"file not found")
                else:
                    s.expect(b"cat work/double.py\r", serial.SHELL, RIGHT.encode())
                    s.expect(b"python work/message.py\r", serial.SHELL, b"\nready\n")
            s.send(f'agent run /work "{PROJECT_BUDGET}"\r'.encode())
            output = s.until(confirm, timeout=90)
            assert b'agent: [50/50] read "budget.py"' in output, output
            assert b"Finished after 50 tool calls." in output, output
            s.expect(b"n", serial.SHELL, b"discarded staged scripts")
            s.send(b"tcc /src/agent/run.c -o /bin/agent-run\r")
            output = s.until(serial.SHELL)
            assert b"shell:" not in output and b"error:" not in output, output
            s.send(f'agent tune "{PROJECT_TUNE}"\r'.encode())
            output = s.until(confirm)
            assert b'Settings ready.' in output, output
            s.expect(b"y", serial.SHELL, b"applied settings for the next session")
            s.send(b"shutdown\r")
            s.until(b"Shutting down LixinOS")
            assert s.vm.wait(timeout=10) == 0

        with serial.boot(disk, command) as s:
            s.expect(b'agent run /agent-policy "change settings"\r', serial.SHELL,
                     b"use agent tune for the agent policy")
            s.expect(b"echo " + b"p" * 96 + b" > work/preview.txt\r", serial.SHELL)
            s.send(f'agent run /work "{PROJECT_TUNED_LIMIT}"\r'.encode())
            output = s.until(serial.SHELL)
            assert b"agent: read 97 bytes (preview)" in output, output
            assert b"p" * 64 + b"\n[truncated]\n" in output, output
            assert b"p" * 65 not in output, output
            assert b'agent: [03/3] list "."' in output, output
            assert b"project agent reached its tool limit" in output, output
            assert b"agent: [04/" not in output, output
            s.expect(b"agent rollback-tune\r", serial.SHELL, b"restored previous version")
            s.expect(f'echo "{GIT_REFERENCE_MARKER}" >> /src/git/README.md\r'.encode(), serial.SHELL)
            s.expect(f'agent run /work "{PROJECT_CHECK}"\r'.encode(), serial.SHELL,
                     b"The rebuilt harness read the saved script.")
            s.expect(b'agent run / "edit root"\r', serial.SHELL, b"use agent system")
            for answer in (b"n", b"y"):
                s.send(f'agent system "{SYSTEM_UPDATE}"\r'.encode())
                output = s.until(b"Apply these system filesystem changes? [y/N] ")
                assert b'+++ proposed "/bin/agent-demo": file' in output, output
                assert b'+++ proposed "/work/note": missing' in output, output
                assert b'+ changed' in output, output
                if answer == b"n":
                    s.expect(answer, serial.SHELL, b"discarded staged system changes")
                    s.expect(b"cat work/preview.txt\r", serial.SHELL, b"p" * 96)
                    s.expect(b"cat /bin/agent-demo\r", serial.SHELL, b"file not found")
                else:
                    s.expect(answer, serial.SHELL, b"applied system changes")
                    s.expect(b"agent-demo\r", serial.SHELL, b"Guest build")
            s.send(b"shutdown\r")
            s.until(b"Shutting down LixinOS")
            assert s.vm.wait(timeout=10) == 0

        with serial.boot(disk) as s:
            s.expect(b"agent-demo\r", serial.SHELL, b"Guest build")
            s.expect(b"cat work/preview.txt\r", serial.SHELL, b"changed")
            s.expect(b"cat work/note\r", serial.SHELL, b"file not found")
            s.expect(b"touch keep-this\r", serial.SHELL)
            s.expect(b"echo manual > work/preview.txt\r", serial.SHELL)
            s.expect(b"agent system-rollback\r", serial.SHELL, b"a file changed after the system update")
            s.expect(b"cat work/preview.txt\r", serial.SHELL, b"manual")
            s.expect(b"agent-demo\r", serial.SHELL, b"Guest build")
            s.expect(b"echo changed > work/preview.txt\r", serial.SHELL)
            s.expect(b"agent system-rollback\r", serial.SHELL, b"restored the previous system files")
            s.expect(b"cat /bin/agent-demo\r", serial.SHELL, b"file not found")
            s.expect(b"cat work/preview.txt\r", serial.SHELL, b"p" * 96)
            s.expect(b"ls -1 work\r", serial.SHELL, b"note\n")
            s.expect(b"ls -1 /\r", serial.SHELL, b"keep-this\n")
            s.expect(b"agent system-rollback\r", serial.SHELL, b"no saved system change")


if __name__ == "__main__":
    if sys.argv[1:2] == ["--model-fixture"]:
        complete = partial(mock_deepseek, directory_count=int(sys.argv[2]))
        sys.exit(bridge.launch(sys.argv[3:], bridge.DeepSeek("fixture-key", "fixture", complete)))
    check_chunked_bridge()
    test()
    print("Passed QEMU inspection, tuning, system changes, reboot, and rollback.")
