"""Drive the desktop agent with a deterministic model and test its guest releases."""
import argparse
import gzip
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import runpy
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from unittest.mock import patch

from disk_image import snapshot_headers

HERE = Path(__file__).resolve().parent
SERIAL = runpy.run_path(str(HERE / "test-user.py"))
BRIDGE = runpy.run_path(str(HERE / "agent-bridge.py"))
DECODE = runpy.run_path(str(HERE / "export-git.py"))["decode"]
SHELL = SERIAL["SHELL"]
PROMPT = b"\x1b[96magent> \x1b[0m"
PROJECT = "/work/toggle"
PREFIX = ["python3", "-I", "/src/agent/desktop.py"]
BADGE = "    app_rect(s, 2, 2, 4, 4, app_rgb(245, 107, 102));"
DRAFT_CASE = ('[{"name":"badge","steps":[{"event":["resize",64,42],'
              '"pixels":[[2,2,62284]]}]}]')
RECHECK_TASK = "Keep requesting checks after a passing result"
RECHECK_SUMMARY = "Stopped after a repeated check. The unchanged draft has a passing candidate for /publish."


TIMER = "/work/focus-timer"
TIMER_TASK = "Pause the timer with Space and preserve state across resize"
TIMER_HANDLER = """    s->timer_ms = 50;
    if (e.type == WINDOW_CLIENT_TIMER) ++ticks;
    return e.type == WINDOW_CLIENT_TIMER;"""
TIMER_SOURCE = """#include "/src/desktop/window-app.h"
static int ticks, running = 1;
static int event(AppWindow *s, WindowEvent e) {
""" + TIMER_HANDLER + """
}
static void draw(AppWindow *s) {
    app_rect(s, 0, 0, s->width, s->height, ticks % 2 ? 65535 : 0);
}
int main(void) { return app_run(event, draw); }
"""
TIMER_FIX = """    if (e.type == WINDOW_CLIENT_KEY && e.value == 32) running = !running;
    s->timer_ms = running ? 50 : 0;
    if (e.type == WINDOW_CLIENT_TIMER && running) ++ticks;
    return e.type == WINDOW_CLIENT_TIMER;"""
TIMER_BASE = [{"name": "timer ticks", "steps": [
    {"event": ["resize", 64, 42], "timer_ms": 50, "pixels": [[0, 0, 0]]},
    {"event": ["timer"], "timer_ms": 50, "pixels": [[0, 0, 65535]]},
]}]
TIMER_NEW = [{"name": "timer pauses", "steps": [
    {"event": ["resize", 64, 42], "timer_ms": 50},
    {"event": ["key", 32], "timer_ms": 0},
    {"event": ["resize", 80, 48], "timer_ms": 0, "pixels": [[79, 47, 0]]},
]}]


def verify_model_tools():
    guide = (HERE.parent / "User/agent/desktop.md").read_text()

    def call(name, args):
        return {"type": "function", "function": {"name": name, "arguments": json.dumps(args)}}

    def send(*calls):
        def open_url(request, timeout):
            payload = json.loads(request.data)
            assert payload["tool_choice"] == "required" and "response_format" not in payload, payload
            assert {tool["function"]["name"] for tool in payload["tools"]} == {
                "read_file", "write_file", "apply_patch", "app_status", "check_app", "finish",
            }
            return io.BytesIO(json.dumps({"choices": [{"finish_reason": "tool_calls",
                "message": {"content": None, "tool_calls": calls}}]}).encode())

        provider = BRIDGE["DeepSeek"]("fixture", "fixture", open_url)
        return BRIDGE["dispatch"](b"chat-session", [guide, "task", PROJECT, "50", "", "", "", ""], provider)

    assert send(call("read_file", {"target": "sdk", "start": 40, "end": 200}),
                call("check_app", {})) == ["read_file", "sdk", "40", "79"]
    assert send(call("apply_patch", {"new": "replacement", "old": "original", "target": "source"})) == [
        "apply_patch", "source", "original", "replacement",
    ]
    assert send(call("finish", {"summary": "Ready to publish."})) == ["finish", "Ready to publish."]
    for invalid in (call("exec", {}), call("check_app", {"extra": True}),
                    call("read_file", {"target": "sdk", "start": True, "end": 10}),
                    call("read_file", {"target": "sdk", "start": 5}),
                    call("write_file", {"target": "source", "text": "好" * 1400})):
        try:
            send(invalid)
        except BRIDGE["ModelReplyError"]:
            continue
        raise AssertionError(invalid)


def verify_interrupted_write_and_compaction():
    workbench = runpy.run_path(str(HERE.parent / "User/agent/desktop.py"))
    worker = runpy.run_path(str(HERE.parent / "User/agent/native.py"))
    assert not worker["desktop_tool"]("exec", [*PREFIX, "write"])
    with tempfile.TemporaryDirectory(prefix="lixin-journal-") as directory:
        root = Path(directory)
        turn = ".agent-trace-1234567890-0001.jsonl"
        events = [
            {"event": "turn_started", "data": {"project": str(root), "mode": "desktop", "task": "Edit the draft"}},
            {"event": "tool_started", "workspace": {"source": "before"},
             "data": {"step": 1, "tool": "exec", "target": "patch",
                      "intent": {"file": "source", "before": "before", "after": "after"}}},
        ]
        with (root / turn).open("w") as stream:
            for seq, event in enumerate(events, 1):
                stream.write(json.dumps(dict(schema=2, seq=seq, session_id=turn, **event)) + "\n")
            stream.write('{"event":"tool_finished"')

        class Project:
            state = ".fixture-release"

            def active(self):
                return {"current": None}

            def candidate(self, active):
                raise ValueError("no passing candidate")

        project = Project()
        project.root = root
        resume = workbench["resume_info"]
        for current, expected in (
            ("before", "file still matches the pre-write state"),
            ("after", "expected write is on disk"),
            ("changed", "file differs from both saved fingerprints"),
        ):
            with patch.dict(resume.__globals__, trace_fingerprint=lambda path: current if path.name == "app.c" else None):
                result = resume(project, turn)
            assert result["continue_turn"] and expected in result["context"], result

    saved = []
    journal = type("Journal", (), {"emit": lambda self, event, **data: saved.append((event, data))})()
    observations = [json.dumps(dict(operation="check" if i == 1 else "read",
                                    status="failed" if i == 1 else "ok", detail=f"result {i}"))
                    for i in range(16)]
    selected, cursor = worker["desktop_observations"](observations, journal, None)
    assert len(observations) == 16 and len(selected) == 9 and selected[-1] == observations[-1]
    assert json.loads(selected[0])["last_check"] == "result 1", selected
    worker["desktop_observations"](observations, journal, cursor)
    assert saved == [("context_compacted", {"omitted": 8, "retained": 8})], saved


def traces(disk, project=PROJECT):
    with Path(disk).open("rb") as source:
        _, base, size, _, header = snapshot_headers(source)[0]
        source.seek(base + 512)
        files = DECODE(source.read(size), header[:8])
    result = {}
    for path, contents in files.items():
        if path.parent != PurePosixPath(project) or not path.name.startswith(".agent-trace-"):
            continue
        assert contents and contents.endswith(b"\n"), path
        events = [json.loads(line) for line in contents.splitlines()]
        elapsed = -1
        for seq, event in enumerate(events, 1):
            assert event["schema"] == 2 and event["seq"] == seq, event
            assert event["turn_id"] == path.name and event["session_id"] == events[0]["session_id"], event
            if event["event"] in ("tool_started", "tool_finished"):
                assert event["data"]["step"] == event["data"]["call_id"], event
            assert event["elapsed_ms"] >= elapsed, event
            elapsed = event["elapsed_ms"]
        assert events[0]["event"] == "turn_started" and events[0]["data"]["mode"] == "desktop"
        result.setdefault(events[0]["data"]["task"], []).append(events)
    return result


def verify_traces(disk):
    saved = traces(disk)
    build, = saved["Build the switch"]
    assert build[-1]["event"] == "turn_finished" and build[-1]["data"]["status"] == "ok", build
    models = [entry["data"] for entry in build if entry["event"] == "model_finished"]
    assert models[0]["result"]["status"] == "failed" and models[1]["attempt"] == 2, models
    checks = [entry["data"] for entry in build if entry["event"] == "tool_finished"
              and entry["data"].get("target") == "check"]
    assert checks[0]["result"]["exit_code"] != 0 and checks[0]["duration_ms"] >= 0, checks
    assert checks[1]["result"]["exit_code"] == 0 and "CHECK PASSED" in checks[1]["result"]["detail"], checks
    badge, = saved["Add a red badge"]
    assert any("PASS regression toggle" in e["data"].get("result", {}).get("detail", "") for e in badge)
    recheck, = saved[RECHECK_TASK]
    assert recheck[-1]["event"] == "turn_finished" and recheck[-1]["data"]["status"] == "ok", recheck
    assert recheck[-2]["data"]["text"] == RECHECK_SUMMARY, recheck
    checks = [e["data"]["result"]["detail"] for e in recheck
              if e["event"] == "tool_finished" and e["data"].get("target") == "check"]
    assert len(checks) == 3 and all("CHECK PASSED 2 cases" in result for result in checks[:2]), checks
    assert "CHECK REUSED 2 cases" in checks[2] and "Building with TCC" not in checks[2], checks
    assert checks[1].split("| candidate ")[1].strip() == checks[2].split("| candidate ")[1].strip(), checks
    long_reply, = saved["Return a long reply"]
    assert long_reply[-2]["event"] == "assistant", long_reply
    assert long_reply[-2]["data"]["text"].startswith("Long reply starts here"), long_reply
    for label in ("/publish", "/rollback"):
        assert any(event["event"] == "tool_finished" and event["data"]["result"]["exit_code"] == 0
                   for run in saved[label] for event in run)
    for runs in saved.values():
        assert all(run[-1]["event"] == "turn_finished" for run in runs)


def timer_reply(data):
    assert data["workspace"] == TIMER
    step = len(data["observations"])

    def tool(*args):
        return {"tool": "exec", "args": [*PREFIX, *args]}

    def finish(text):
        return {"tool": "finish", "args": [text]}

    if data["task"] == "Build the timer":
        actions = [tool("write", "source", TIMER_SOURCE),
                   tool("write", "tests", json.dumps(TIMER_BASE)), tool("check")]
        if step < len(actions):
            return actions[step]
        assert "CHECK PASSED 1 cases" in data["observations"][-1]["detail"], data
        return finish("The timer is ready.")
    assert data["task"] == TIMER_TASK, data
    context = data["conversation"]
    previous = json.loads(context.split("\n", 1)[1]) if context.startswith("Saved resume context") else {}
    if previous.get("task") != TIMER_TASK:
        if step == 0:
            return tool("write", "tests", json.dumps(TIMER_NEW))
        if step == 1:
            return tool("check")
        assert "timer pauses step 2" in data["observations"][-1]["detail"], data
        disk = Path(os.environ["LIXINOS_DISK"])
        saved, = traces(disk, TIMER)[TIMER_TASK]
        assert saved[-1]["event"] == "model_started" and saved[-2]["event"] == "tool_finished", saved
        disk.with_suffix(".checkpoint-ready").touch()
        time.sleep(100)
        raise AssertionError("the test should have powered off QEMU")
    assert "timer pauses step 2" in context and '"matches_draft":false' in context, context
    if step == 0:
        disk = Path(os.environ["LIXINOS_DISK"])
        if not disk.with_suffix(".resume-ready").exists():
            assert "changed: source" in context, context
            disk.with_suffix(".resume-ready").touch()
            time.sleep(100)
            raise AssertionError("the test should have interrupted the first resume")
        return tool("read", "source", "10", "15")
    if step == 1:
        assert "operator edit" in data["observations"][-1]["detail"], data
        return tool("patch", "source", TIMER_HANDLER, TIMER_FIX)
    if step == 2:
        return tool("check")
    detail = data["observations"][-1]["detail"]
    assert all(marker in detail for marker in ("PASS regression timer ticks",
               "PASS draft timer pauses", "CHECK PASSED 2 cases")), detail
    return finish("Resumed timer passed both cases. Ready for /publish.")


def reply(instruction, data):
    assert "LixinOS desktop app agent" in instruction, instruction
    if data["workspace"] == TIMER:
        return timer_reply(data)
    assert data["workspace"] == PROJECT
    observations = data["observations"]

    def tool(*args):
        return {"tool": "exec", "args": [*PREFIX, *args]}

    def finish(summary):
        return {"tool": "finish", "args": [summary]}

    detail = observations[-1]["detail"] if observations else ""
    step = len(observations)
    if data["task"] == "Build the switch":
        if step == 0:
            recorded, = traces(os.environ["LIXINOS_DISK"])["Build the switch"]
            assert recorded[-1]["event"] == "model_started", recorded
            if recorded[-1]["data"]["attempt"] == 1:
                raise BRIDGE["ModelReplyError"]("invalid fixture model reply")
            return {"tool": "exec", "args": ["sh", "-c", "touch /escaped"]}
        if step == 1:
            assert "only workbench tools" in detail, observations
            return tool("publish")
        if step == 2:
            assert "only workbench tools" in detail, observations
            return tool("patch", "source", "int main", "badtoken int main")
        if step == 3:
            return tool("check")
        if step == 4:
            assert observations[-1]["status"] == "failed" and "error" in detail, observations
            return tool("patch", "source", "badtoken int main", "int main")
        if step == 5:
            return tool("check")
        assert "CHECK PASSED 1 cases" in detail, observations
        return finish("The switch is ready.")
    if data["task"] == "Add a red badge":
        assert "The switch is ready." in data["conversation"], data
        if step == 0:
            return tool("write", "tests", DRAFT_CASE)
        if step == 1:
            return tool("check")
        if step == 2:
            assert "PASS regression toggle" in detail and "badge step 1" in detail, observations
            return tool("patch", "source", "    app_text(", BADGE + "\n    app_text(")
        if step == 3:
            return tool("check")
        assert "CHECK PASSED 2 cases" in detail, observations
        return finish("The red badge passed both cases.")
    if data["task"] == RECHECK_TASK:
        if step == 1:
            assert "CHECK PASSED 2 cases" in detail, observations
            return tool("patch", "source", BADGE, BADGE + " /* badge */")
        if step == 3:
            assert "CHECK PASSED 2 cases" in detail, observations
        assert step < 4, observations
        return {"tool": "check_app", "args": []}
    if data["task"] == "Return a long reply":
        return finish("Long reply starts here\n" + ('"a' * 1850))
    raise AssertionError(data["task"])


def run(guest, command, prompt=SHELL, *expected):
    guest.send(command.encode() + b"\r")
    output = guest.until(prompt, timeout=90)
    for marker in expected:
        assert marker in output, output
    return output


def active(output):
    return re.search(rb"ACTIVE ([a-f0-9]{12}) /bin/toggle.win2", output)[1]


def shutdown(guest):
    guest.send(b"q\r")
    assert guest.vm.wait(timeout=15) == 0


def verify_resume(disk, boot):
    def power_off_at(guest, marker):
        ready = disk.with_suffix(marker)
        deadline = time.monotonic() + 90
        while not ready.exists() and guest.vm.poll() is None and time.monotonic() < deadline:
            time.sleep(0.05)
        assert ready.exists(), guest.buffer
        guest.vm.terminate()
        guest.vm.wait(timeout=10)

    with SERIAL["boot"](disk, boot) as guest:
        run(guest, f"agent desktop new {TIMER}", SHELL, b"Created")
        run(guest, f"agent desktop {TIMER}", PROMPT, b"Desktop workbench")
        run(guest, "Build the timer", PROMPT, b"The timer is ready.")
        baseline = re.search(rb"ACTIVE ([a-f0-9]{12}) /bin/focus-timer.win2",
                             run(guest, "/publish", PROMPT))[1]
        guest.send(TIMER_TASK.encode() + b"\r")
        power_off_at(guest, ".checkpoint-ready")

    with SERIAL["boot"](disk, boot) as guest:
        run(guest, f"echo '/* operator edit */' >> {TIMER}/app.c", SHELL)
        guest.send(f"agent resume {TIMER}\r".encode())
        first_summary = guest.until(b"timer pauses step 2", timeout=90)
        assert b"Workspace: changed: source" in first_summary, first_summary
        power_off_at(guest, ".resume-ready")

    with SERIAL["boot"](disk, boot) as guest:
        output = run(guest, f"agent resume {TIMER}", PROMPT, b"Last check (failed)",
                     b"Workspace: unchanged", b"timer pauses step 2",
                     b"Resumed timer passed both cases.")
        assert b"ACTIVE " not in output, output
        run(guest, "/status", PROMPT, b"current: " + baseline, b"ready to publish")
        run(guest, "/exit")
        interrupted, retry, resumed = sorted(traces(disk, TIMER)[TIMER_TASK], key=lambda run: run[0]["turn_id"])
        assert interrupted[0]["session_id"] == resumed[0]["session_id"]
        assert interrupted[-1]["event"] == retry[-1]["event"] == "model_started"
        assert retry[0]["data"]["resumed_from"] == interrupted[0]["turn_id"]
        assert resumed[0]["data"]["resumed_from"] == retry[0]["turn_id"]
        run(guest, f'agent resume {TIMER} {TIMER}/{resumed[0]["turn_id"]}', PROMPT,
            b"Workspace: unchanged", b"Candidate: ready to publish")
        run(guest, "/exit")
        shutdown(guest)


def test(kernel, release):
    with tempfile.TemporaryDirectory(prefix="lixin-desktop-agent-", dir="/tmp") as tmp:
        tmp = Path(tmp)
        disk = tmp / "desktop.img"
        if kernel == "tcc":
            artifact = f"refs/tags/{release}:kernel-artifacts/tcc-system.img.gz"
            with subprocess.Popen(["git", "show", artifact], cwd=HERE.parent,
                                  stdout=subprocess.PIPE) as archive:
                with gzip.GzipFile(fileobj=archive.stdout) as compressed, disk.open("wb") as output:
                    shutil.copyfileobj(compressed, output)
                assert archive.wait() == 0
        qemu = shutil.which("qemu-system-aarch64")
        wrapper = tmp / "qemu-system-aarch64"
        wrapper.write_text("#!/bin/sh\nexec " + shlex.join(
            [sys.executable, str(Path(__file__).resolve()), "--fixture", qemu]) + ' "$@"\n')
        wrapper.chmod(0o700)
        boot = ["env", f"PATH={tmp}:{os.environ['PATH']}", HERE / "run-kernel.sh",
                "--kernel", kernel, "--serial", *(["--release", release] if release else ["--source"])]
        with SERIAL["boot"](disk, boot) as guest:
            run(guest, f"agent desktop new {PROJECT}", SHELL, b"Created /work/toggle")
            run(guest, f"agent desktop {PROJECT}", PROMPT, b"Desktop workbench")
            run(guest, "/publish", PROMPT, b"no passing candidate")
            run(guest, "Build the switch", PROMPT, b"The switch is ready.")
            version1 = active(run(guest, "/publish", PROMPT))
            run(guest, "Add a red badge", PROMPT, b"The red badge passed both cases.")
            run(guest, RECHECK_TASK, PROMPT, b"CHECK REUSED 2 cases", RECHECK_SUMMARY.encode())
            version2 = active(run(guest, "/publish", PROMPT))
            assert version1 != version2
            output = run(guest, "Return a long reply", PROMPT, b"Long reply starts here")
            assert b"trace stopped" not in output, output
            run(guest, "/exit")
            run(guest, "stat /bin/toggle.win2", SHELL, b"regular file")
            shutdown(guest)

        with SERIAL["boot"](disk, boot) as guest:
            run(guest, f"agent desktop {PROJECT}", PROMPT)
            run(guest, "/trace", PROMPT, b"/work/toggle/.agent-trace-")
            run(guest, "/status", PROMPT, b"current: " + version2, b"previous: " + version1)
            assert active(run(guest, "/rollback", PROMPT)) == version1
            run(guest, "/status", PROMPT, b"current: " + version1)
            run(guest, "/exit")
            run(guest, f"agent trace {PROJECT}", SHELL, b"/work/toggle/.agent-trace-")
            shutdown(guest)
        verify_traces(disk)
        verify_resume(disk, boot)


if __name__ == "__main__":
    if sys.argv[1:2] == ["--fixture"]:
        signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
        sys.exit(BRIDGE["launch"](sys.argv[2:], reply))
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", choices=("rust", "tcc"), default="rust")
    parser.add_argument("--release")
    options = parser.parse_args()
    if options.kernel == "tcc" and not options.release:
        parser.error("--kernel tcc requires --release TAG")
    if options.release == "latest":
        options.release = subprocess.check_output(
            [sys.executable, HERE / "kernel-release.py", "latest"], text=True).strip()
    verify_model_tools()
    verify_interrupted_write_and_compaction()
    test(options.kernel, options.release)
    print(f"Verified {options.kernel} desktop agent retries, regression suite, publish, rollback, live traces and task recovery across reboot.")
