"""Exercise the real QEMU bridge and guest Agent with deterministic model responses."""
import importlib.util
from io import BytesIO
import json
from pathlib import Path
import sys
import tempfile
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
INSPECT = "Report CPU, RAM, disk, filesystem, and the file in the current directory"
UNSUPPORTED = "Try to remove sample.txt, then check whether it still exists"
FILE_CONTENT = b"guest-private-content"


def mock_inspect(instruction, data):
    assert "LixinOS read-only system inspector" in instruction
    assert data["cwd"] == "/lab"
    observations = data["observations"]
    assert data["remaining_queries"] == 6 - len(observations)
    assert all(FILE_CONTENT.decode() not in item for item in observations)
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
        ({"tool": "cpu"}, ("vcpus=1", "uptime_seconds=", "guest_busy_percent=unavailable")),
        ({"tool": "memory"}, ("guest_ram_bytes=167772160", "heap_used_bytes=", "heap_free_bytes=")),
        ({"tool": "disk"}, ("capacity_bytes=9437184", "committed_snapshot_generation=3")),
        ({"tool": "filesystem"}, ("files=1", "directories=2", "file_content_bytes=22")),
        ({"tool": "list", "path": "."}, ('path="/lab"', 'file "sample.txt" (22 bytes)')),
        ({"tool": "stat", "path": "sample.txt"}, ('path="/lab/sample.txt"', "size_bytes=22")),
    ]
    for observation, (tool, evidence) in zip(observations, tools):
        assert f'tool="{tool["tool"]}"' in observation, observation
        assert all(value in observation for value in evidence), observation
    if not observations:
        return {"queries": [tool for tool, _ in tools[:4]]}
    if len(observations) == 4:
        return {"queries": [tool for tool, _ in tools[4:]]}
    return {"answer": "1 vCPU, 160 MiB of RAM, 9 MiB disk, and sample.txt is 22 bytes.\x1b[2J"}


def mock_deepseek(request, timeout):
    assert request.full_url == bridge.ENDPOINT
    assert request.get_header("Authorization") == "Bearer fixture-key"
    payload = json.loads(request.data)
    assert timeout == 45 and payload["response_format"] == {"type": "json_object"}
    instruction, content = payload["messages"]
    data = json.loads(content["content"])
    if "question" in data:
        result = mock_inspect(instruction["content"], data)
    elif data["task"] == "reject with HTTP error":
        raise urllib.error.HTTPError(request.full_url, 401, "Fixture error", {}, BytesIO())
    elif bridge.TEST_PROMPT in instruction["content"]:
        result = {"tests": TESTS}
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


def test():
    with tempfile.TemporaryDirectory() as temporary:
        disk = Path(temporary) / "agent.img"
        with serial.boot(disk) as s:
            s.expect(b'agent inspect "How much RAM?"\r', serial.SHELL, b"model bridge unavailable")
            s.expect(b'agent offline.py "demo"\r', serial.SHELL, b"model bridge unavailable")
            s.expect(b"cat offline.py\r", serial.SHELL, b"file not found")
        with disk.open("r+b") as image:
            image.truncate(9 * 1024 * 1024)

        qemu = [
            "qemu-system-aarch64", "-machine", "virt", "-accel", "hvf", "-cpu", "host",
            "-smp", "1", "-m", "160M", "-display", "none", "-serial", "stdio", "-monitor", "none",
            "-global", "virtio-mmio.force-legacy=false",
            "-drive", f"if=none,id=fs,format=raw,cache=writethrough,file={disk}",
            "-device", "virtio-blk-device,drive=fs,bus=virtio-mmio-bus.0",
            "-kernel", str(serial.ROOT / "Kernel/target/aarch64-unknown-none-softfloat/release/lixin-kernel"),
        ]
        command = [sys.executable, __file__, "--model-fixture", *qemu]
        with serial.boot(disk, command) as s:
            s.expect(b"mkdir lab\r", serial.SHELL)
            s.expect(b"echo " + FILE_CONTENT + b" > lab/sample.txt\r", serial.SHELL)
            s.expect(b"cd lab\r", b"lixin:/lab$ ")
            disk_before = disk.read_bytes()
            s.send(f'agent inspect "{INSPECT}"\r'.encode())
            output = s.until(b"lixin:/lab$ ")
            assert b"1 vCPU, 160 MiB of RAM, 9 MiB disk, and sample.txt is 22 bytes." in output, output
            assert b"\x1b[2J" not in output, output
            s.expect(f'agent inspect "{UNSUPPORTED}"\r'.encode(), b"lixin:/lab$ ", b"sample.txt still exists")
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


if __name__ == "__main__":
    if sys.argv[1:2] == ["--model-fixture"]:
        sys.exit(bridge.launch(sys.argv[2:], bridge.DeepSeek("fixture-key", "fixture", mock_deepseek)))
    test()
    print("Passed QEMU inspection, model bridge, repair, guest test limits, and offline reuse after reboot.")
