"""LixinOS desktop app workbench. Run with python3 -I."""
import hashlib
import json
import os
from pathlib import Path
import re
import select
import struct
import subprocess
import sys
import tempfile
import time


TEMPLATE = Path("/src/desktop/app-template.c")
GUIDE = Path("/src/agent/desktop.md")
WORK = Path(".desktop")
SOURCE = Path("app.c")
TESTS = Path("tests.json")
CANDIDATE = WORK / "candidate"
MAX_TEXT = 32768
MAX_CASES = 64
NAME = re.compile(r"[a-z][a-z0-9-]{0,30}")
DIGEST = re.compile(r"[0-9a-f]{64}")
EVENT = struct.Struct("<hhhh")
UPDATE = struct.Struct("<5H")
EVENTS = {"pointer": (0, 3), "key": (1, 1), "resize": (2, 2), "timer": (3, 0)}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def rgb(r, g, b):
    return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3


def json_bytes(data):
    return (json.dumps(data, sort_keys=True, indent=2) + "\n").encode()


def read_small(path):
    with path.open("rb") as stream:
        data = stream.read(MAX_TEXT + 1)
    if len(data) > MAX_TEXT:
        raise ValueError(f"{path} is larger than {MAX_TEXT} bytes")
    return data


def write_atomic(path, data):
    if Path.cwd().parent == Path("/work/store") and Path("/lib/app-store/save").is_file():
        subprocess.run(["/lib/app-store/save", str(path), str(len(data))], input=data, check=True)
        return
    with tempfile.TemporaryDirectory(dir=path.parent) as folder:
        temporary = Path(folder) / "file"
        temporary.write_bytes(data)
        temporary.replace(path)


def choose_root(root):
    if (not NAME.fullmatch(root.name) or root.parts[1] in ("bin", "src", "lib", "boot", "libexec", "proc", "sys", "agent-policy")
            or ".git" in root.parts):
        raise ValueError("choose a project such as /work/my-app")
    return root


def create(path):
    root = Path(os.path.abspath(path))
    if not NAME.fullmatch(root.name):
        raise ValueError("use lowercase letters, digits and hyphens for the app name")
    choose_root(root)
    root.mkdir(parents=True, exist_ok=True)
    os.chdir(root)
    if any(root.iterdir()):
        raise ValueError("choose an empty project directory")
    SOURCE.write_bytes(TEMPLATE.read_bytes())
    off, on, background = rgb(79, 110, 177), rgb(43, 190, 137), rgb(22, 31, 43)
    suite = [{"name": "toggle", "steps": [
        {"event": ["resize", 64, 42], "pixels": [[9, 13, off], [0, 0, background]]},
        {"event": ["pointer", 9, 13, 1], "pixels": [[9, 13, on]]},
        {"event": ["pointer", 9, 13, 0]},
        {"event": ["key", 32], "pixels": [[9, 13, off]]},
        {"event": ["resize", 80, 48], "pixels": [[69, 13, off]]},
    ]}]
    TESTS.write_bytes(json_bytes(suite))
    WORK.mkdir()
    print(f"Created {root}. Start with agent desktop {root}")


def cases(data):
    suite = json.loads(data)
    if not isinstance(suite, list) or not 1 <= len(suite) <= MAX_CASES:
        raise ValueError(f"provide 1–{MAX_CASES} test cases")
    for case in suite:
        name, steps = case["name"], case["steps"]
        if not isinstance(name, str) or not re.fullmatch(r"[a-zA-Z0-9 ._-]{1,64}", name):
            raise ValueError("use a short test case name")
        if not isinstance(steps, list) or not 1 <= len(steps) <= 8:
            raise ValueError(f"{name}: provide 1–8 steps")
        if steps[0]["event"][:1] != ["resize"]:
            raise ValueError(f"{name}: first event must be resize")
        width, height = 0, 0
        for step in steps:
            event, *args = step["event"]
            if event not in EVENTS or len(args) != EVENTS[event][1]:
                raise ValueError(f"{name}: invalid event")
            if any(type(value) is not int or not -32768 <= value <= 32767 for value in args):
                raise ValueError(f"{name}: invalid event argument")
            if event == "resize":
                width, height = args
                if not 16 <= width <= 80 or not 16 <= height <= 48:
                    raise ValueError(f"{name}: use a test canvas of 16–80 by 16–48 pixels")
            pixels = step.get("pixels", [])
            if not isinstance(pixels, list) or len(pixels) > 16:
                raise ValueError(f"{name}: use up to 16 pixel assertions per step")
            for x, y, color in pixels:
                if any(type(value) is not int for value in (x, y, color)) or not (
                    0 <= x < width and 0 <= y < height and 0 <= color <= 65535
                ):
                    raise ValueError(f"{name}: invalid pixel assertion")
            delay = step.get("timer_ms", 0)
            if type(delay) is not int or not 0 <= delay <= 65535:
                raise ValueError(f"{name}: invalid timer_ms")
    return suite


def read_reply(fd, length, deadline):
    data = bytearray()
    while len(data) < length:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([fd], [], [], remaining)[0]:
            raise ValueError("window client did not reply in time")
        block = os.read(fd, min(4096, length - len(data)))
        if not block:
            break
        data.extend(block)
    return data


def pack_event(event):
    kind, *args = event
    if kind == "key":
        return EVENT.pack(EVENTS[kind][0], 0, 0, args[0])
    return EVENT.pack(EVENTS[kind][0], *(args + [0] * (3 - len(args))))


def run_case(binary, case):
    child = subprocess.Popen([str(binary.absolute())], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    deadline = time.monotonic() + 3
    width, height, surface = 0, 0, bytearray()
    try:
        # The desktop sends only one event at a time and waits for its reply.
        fd = child.stdout.fileno()
        for index, step in enumerate(case["steps"], 1):
            event = step["event"]
            try:
                child.stdin.write(pack_event(event))
                child.stdin.flush()
                header = read_reply(fd, UPDATE.size, deadline)
                if len(header) != UPDATE.size:
                    raise ValueError("missing or incomplete window reply")
                x, y, w, h, delay = UPDATE.unpack(header)
                if event[0] == "resize":
                    width, height = event[1:]
                    surface = bytearray(width * height * 2)
                    if (x, y, w, h) != (0, 0, width, height):
                        raise ValueError("resize did not repaint the whole canvas")
                elif (w == 0) != (h == 0) or x + w > width or y + h > height:
                    raise ValueError("window update falls outside the canvas")
                image = read_reply(fd, w * h * 2, deadline)
                if len(image) != w * h * 2:
                    raise ValueError("window reply has incomplete pixels")
                for row in range(h):
                    start = ((y + row) * width + x) * 2
                    surface[start:start + w * 2] = image[row * w * 2:(row + 1) * w * 2]
                for px, py, color in step.get("pixels", []):
                    actual = struct.unpack_from("<H", surface, (py * width + px) * 2)[0]
                    if actual != color:
                        raise ValueError(f"pixel ({px},{py}) is {actual}, expected {color}")
                if "timer_ms" in step and step["timer_ms"] != delay:
                    raise ValueError(f"timer is {delay} ms, expected {step['timer_ms']} ms")
            except (OSError, ValueError) as error:
                raise ValueError(f"{case['name']} step {index} ({event[0]}): {error}") from error
        child.stdin.close()
        if read_reply(fd, 1, deadline):
            raise ValueError(f"{case['name']}: unsolicited bytes after the last reply")
        if child.wait(timeout=max(0.05, deadline - time.monotonic())):
            raise ValueError(f"{case['name']}: app exited with status {child.returncode}")
    finally:
        try:
            child.stdin.close()
        except OSError:
            pass
        child.stdout.close()
        if child.poll() is None:
            try:
                child.kill()
                child.wait(timeout=1)
            except (OSError, subprocess.TimeoutExpired):
                pass


class Project:
    def __init__(self):
        self.root = choose_root(Path.cwd())
        self.name = self.root.name
        self.app = self.name + ".win2"
        self.state = "." + self.name + ".desktop-release"
        if not WORK.is_dir() or not SOURCE.is_file() or not TESTS.is_file():
            raise ValueError("initialize this project with the new command")

    def version(self, version):
        if not isinstance(version, str) or not DIGEST.fullmatch(version):
            raise ValueError("invalid release identifier")
        folder = WORK / "versions" / version
        record = json.loads(read_small(folder / "release.json"))
        image = (folder / self.app).read_bytes()
        if (sha(json_bytes(record)) != version or sha(image) != record["binary"]
                or sha(read_small(folder / SOURCE)) != record["source"]
                or sha(read_small(folder / TESTS)) != record["tests"]
                or sha(read_small(folder / "regressions.json")) != record["regressions"]):
            raise ValueError("release archive failed its integrity check")
        return record, image

    def active(self):
        index = Path("/bin") / self.state
        image = Path("/bin") / self.app
        if not index.exists():
            if image.exists():
                raise ValueError("an unmanaged application already uses this name")
            return {"current": None, "previous": None}
        state = json.loads(read_small(index))
        if state["project"] != str(self.root):
            raise ValueError("this desktop app belongs to another project")
        record, _ = self.version(state["current"])
        if not image.is_file() or sha(image.read_bytes()) != record["binary"]:
            raise ValueError("installed app changed outside this project")
        return state

    def check(self, reuse=False):
        if reuse:
            active = self.active()
            try:
                version, record = self.candidate(active)
            except (OSError, ValueError, KeyError, TypeError):
                pass  # Missing, stale or damaged evidence. Run the full check.
            else:
                if record["base"] == active["current"]:
                    print(f"CHECK REUSED {record['cases']} cases | candidate {version[:12]}")
                    return
        CANDIDATE.unlink(missing_ok=True)
        base = self.active()["current"]
        source, tests = read_small(SOURCE), read_small(TESTS)
        suite = [] if base is None else cases(read_small(WORK / "versions" / base / "regressions.json"))
        regressions = len(suite)
        for case in cases(tests):
            if case not in suite:
                suite.append(case)
        cumulative = json_bytes(suite)
        if len(suite) > MAX_CASES or len(cumulative) > MAX_TEXT:
            raise ValueError("regression suite exceeds its size limit")
        with tempfile.TemporaryDirectory(dir=WORK) as stage:
            stage = Path(stage)
            draft, binary = stage / SOURCE, stage / self.app
            draft.write_bytes(source)
            print("Building with TCC", flush=True)
            subprocess.run(["/bin/tcc", str(draft), "-o", str(binary)], check=True)
            image = binary.read_bytes()
            if image[:4] != b"LXE1":
                raise ValueError("TCC did not produce an LXE1 app")
            for index, case in enumerate(suite):
                run_case(binary, case)
                print(f"PASS {'regression' if index < regressions else 'draft'} {case['name']}", flush=True)
            if read_small(SOURCE) != source or read_small(TESTS) != tests:
                raise ValueError("draft changed during build. Run check again")
            result = dict(base=base, source=sha(source), tests=sha(tests), binary=sha(image),
                          regressions=sha(cumulative), cases=len(suite))
            manifest = json_bytes(result)
            version = sha(manifest)
            archive = WORK / "versions" / version
            archive.parent.mkdir(exist_ok=True)
            if not archive.exists():
                (stage / TESTS).write_bytes(tests)
                (stage / "regressions.json").write_bytes(cumulative)
                (stage / "release.json").write_bytes(manifest)
                stage.rename(archive)
            self.version(version)
        write_atomic(CANDIDATE, (version + "\n").encode())
        print(f"CHECK PASSED {len(suite)} cases | candidate {version[:12]}")

    def candidate(self, active=None):
        if not CANDIDATE.is_file():
            raise ValueError("no passing candidate. Run check")
        version = read_small(CANDIDATE).decode().strip()
        record, _ = self.version(version)
        if (sha(read_small(SOURCE)) != record["source"] or sha(read_small(TESTS)) != record["tests"]):
            raise ValueError("draft changed. Run check")
        if active is None:
            active = self.active()
        if active["current"] not in (version, record["base"]):
            raise ValueError("installed version changed. Run check")
        return version, record

    def status(self):
        state = self.active()
        for key in ("current", "previous"):
            print(f"{key}: {state[key][:12] if state[key] else 'none'}")
        try:
            version, record = self.candidate(state)
            note = "installed" if state["current"] == version else "ready to publish"
            print(f"candidate: {version[:12]} ({record['cases']} passing cases, {note})")
        except ValueError as error:
            print(f"candidate: {error}")

    def install(self, version, previous):
        _, image = self.version(version)
        with tempfile.TemporaryDirectory(dir=WORK) as stage:
            stage = Path(stage)
            app, state = stage / self.app, stage / self.state
            app.write_bytes(image)
            state.write_bytes(json_bytes(dict(project=str(self.root), current=version, previous=previous)))
            # Multi-source cp commits both files to /bin in the same transaction.
            subprocess.run(["/bin/cp", str(app), str(state), "/bin"], check=True)
        print(f"ACTIVE {version[:12]} /bin/{self.app}")

    def release(self, operation, expected=None):
        # Serialize operator deployments of the same app, even from another project.
        lock = Path("/bin") / ("." + self.name + ".desktop-lock")
        try:
            lock.mkdir()
        except FileExistsError:
            raise ValueError(f"deployment locked at {lock}") from None
        try:
            state = self.active()
            if operation == "publish":
                version, _ = self.candidate(state)
                if version == state["current"]:
                    print(f"ALREADY ACTIVE {version[:12]} /bin/{self.app}")
                    return
            else:
                version = state["previous"]
                if not version:
                    raise ValueError("no previous release")
            if expected is not None and version != expected:
                raise ValueError("reviewed version changed. Open Review again")
            self.install(version, state["current"])
        finally:
            lock.rmdir()


TRACE_NAME = re.compile(r"\.agent-trace-[0-9]{10}-[0-9]{4}\.jsonl")


def trace_fingerprint(path):
    try:
        data = read_small(path)
    except FileNotFoundError:
        return None
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return f"{len(data)}:{value:08x}"


def trace_events(project, name=None):
    if name is not None:
        path = Path(name)
        if path.parent not in (Path("."), project.root) or not TRACE_NAME.fullmatch(path.name):
            raise ValueError("choose a trace filename in this project")
        paths = [project.root / path.name]
    else:
        paths = sorted((path for path in project.root.iterdir() if TRACE_NAME.fullmatch(path.name)),
                       reverse=True)
    for path in paths:
        try:
            with path.open("rb") as stream:
                first = stream.readline(8193)
                start = json.loads(first)
                data = start["data"]
                if (start["event"] != "turn_started" or start["schema"] not in (1, 2)
                        or data["project"] != str(project.root) or data["mode"] != "desktop"
                        or not isinstance(data["task"], str) or not data["task"].strip()
                        or data["task"].lstrip().startswith("/")):
                    continue
                tail = stream.read(512 * 1024 + 1)
                if len(first) + len(tail) > 512 * 1024:
                    raise ValueError("trace exceeds its size limit")
        except (OSError, ValueError, TypeError, KeyError) as error:
            if name is not None:
                raise ValueError(f"cannot read desktop trace {path.name}") from error
            continue
        events = [start]
        for line in tail.splitlines(keepends=True):
            # An interrupted writer can leave a partial final line. Keep the
            # last complete checkpoint and treat the turn as interrupted.
            if not line.endswith(b"\n"):
                break
            try:
                event = json.loads(line)
                if (not isinstance(event["data"], dict) or not isinstance(event["event"], str)
                        or event["seq"] != len(events) + 1 or event["schema"] != start["schema"]):
                    break
                events.append(event)
            except (ValueError, TypeError, KeyError):
                break
        return path.name, events
    raise ValueError("no resumable desktop turn in this project")


def trace_excerpt(value, length):
    return "".join(char for char in str(value) if char.isprintable() or char == "\n")[:length]


def resume_info(project, name=None):
    turn, events = trace_events(project, name)
    first = events[0]
    task = trace_excerpt(first["data"]["task"], 2048).strip()
    if not task or task.startswith("/"):
        raise ValueError("trace has no resumable task text")
    session = first.get("session_id", turn)
    if not isinstance(session, str) or not TRACE_NAME.fullmatch(session):
        session = turn
    lineage = [(turn, events)]
    task_turns = {turn}
    seen = {turn}
    # A restart during a resumed turn can happen before there are any new
    # tool results. Keep the preceding failure available across such restarts.
    for _ in range(15):
        child_turn, child_events = lineage[-1]
        child = child_events[0]["data"]
        parent = child.get("resumed_from")
        if not isinstance(parent, str) or parent in seen or not TRACE_NAME.fullmatch(parent):
            break
        try:
            parent_turn, parent_events = trace_events(project, parent)
        except ValueError:
            break
        if parent_events[0].get("session_id", parent_turn) != session:
            break
        if child_turn in task_turns and child.get("continues_task", parent_events[0]["data"]["task"] == child["task"]):
            task_turns.add(parent_turn)
        seen.add(parent_turn)
        lineage.append((parent_turn, parent_events))
    live = {
        "source": trace_fingerprint(SOURCE),
        "tests": trace_fingerprint(TESTS),
        "candidate": trace_fingerprint(CANDIDATE),
        "installed": trace_fingerprint(Path("/bin") / project.state),
    }
    active = project.active()
    try:
        candidate, _ = project.candidate(active)
        candidate_state = "installed" if candidate == active["current"] else "ready to publish"
    except (OSError, ValueError, TypeError, KeyError) as error:
        candidate = None
        candidate_state = trace_excerpt(error, 320)

    saved = next((event["workspace"] for event in reversed(events)
                  if isinstance(event.get("workspace"), dict)), None)
    changes = [key for key, value in live.items() if saved is not None and saved.get(key) != value]
    comparison = ("changed: " + ", ".join(changes) if changes else
                  "unchanged" if saved is not None else "no saved fingerprint")

    pending, recent, followups, dialogue = {}, [], [], []
    check, check_workspace = None, None
    for event_turn, event in ((name, record) for name, stage in reversed(lineage) for record in stage):
        data = event["data"]
        kind = event["event"]
        if kind == "turn_started" and event_turn not in task_turns:
            dialogue.append({"user": trace_excerpt(data["task"], 384)})
        elif kind == "assistant" and event_turn not in task_turns and dialogue:
            dialogue[-1]["assistant"] = trace_excerpt(data.get("text", ""), 640)
        elif kind == "follow_up" and event_turn in task_turns:
            followups.append(trace_excerpt(data.get("text", ""), 640))
        if kind not in ("tool_started", "tool_finished"):
            continue
        call = data.get("call_id", data.get("step"))
        if type(call) is not int:
            continue
        target = trace_excerpt(data.get("target") or data.get("tool", ""), 96)
        if kind == "tool_started":
            if event_turn in task_turns:
                pending[event_turn, call] = (target, data.get("intent"))
        else:
            pending.pop((event_turn, call), None)
            result = data.get("result")
            if not isinstance(result, dict):
                continue
            status = trace_excerpt(result.get("status", ""), 48)
            recent.append({"call": call, "target": target, "status": status})
            if event_turn in task_turns and data.get("target") == "check":
                check_workspace = event.get("workspace")
                check = {"status": status, "detail": trace_excerpt(result.get("detail", ""), 720),
                         "exit_code": result.get("exit_code")}
                if isinstance(check_workspace, dict):
                    check["matches_draft"] = all(check_workspace.get(key) == live[key] for key in ("source", "tests"))
    completed = (events[-1]["event"] == "turn_finished" and events[-1]["data"].get("status") == "ok")
    if events[-1]["event"] == "assistant" and not pending:
        completed = True
    check_failed = check is not None and check["status"] != "ok"
    unresolved = []
    for operation, intent in list(pending.values())[-4:]:
        item = {"operation": operation, "state": "inspect before continuing"}
        if isinstance(intent, dict) and intent.get("file") in ("source", "tests"):
            key = intent["file"]
            item["file"] = key
            if live[key] is not None and live[key] == intent.get("after") and live[key] != intent.get("before"):
                item["state"] = "expected write is on disk"
            elif live[key] is not None and live[key] == intent.get("before"):
                item["state"] = "file still matches the pre-write state"
            else:
                item["state"] = "file differs from both saved fingerprints"
        elif operation == "check" and candidate is not None:
            item["state"] = "a passing candidate matches the draft. Inspect status"
        unresolved.append(item)
    continue_turn = not completed or check_failed or any(owner == turn for owner, _ in pending)
    answer = next((trace_excerpt(event["data"].get("text", ""), 640)
                   for _, stage in lineage for event in reversed(stage)
                   if event["event"] == "assistant"), "")
    notes = {
        "resumed_from": turn, "task": trace_excerpt(task, 384), "last_reply": answer,
        "workspace": comparison, "current_release": active["current"],
        "candidate": candidate, "candidate_state": candidate_state, "last_check": check,
        "unfinished_calls": unresolved, "recent_calls": recent[-6:],
        "follow_ups": followups[-3:], "previous_turns": dialogue[-3:],
    }
    summary = f"Resumed {turn}\nTask: {trace_excerpt(task, 300)}\nWorkspace: {comparison}\n"
    if check:
        summary += f"Last check ({check['status']}): {trace_excerpt(check['detail'], 600)}\n"
    if pending:
        summary += "Unfinished tools: " + "; ".join(
            f"{item['operation']}: {item['state']}" for item in unresolved) + "\n"
    summary += f"Candidate: {candidate_state}"
    context = ("Saved resume context (workspace facts, prior outputs and pending actions):\n"
               + json.dumps(notes, ensure_ascii=False, separators=(",", ":")))
    return dict(session_id=session, from_turn=turn, task=task, context=context, summary=summary,
                continue_turn=continue_turn, auto_approve=first["data"].get("auto_approve") is not False)


def panel_info(project, selected="", view="timeline", filenames=()):
    """Bounded, read-only records for the native graphical client."""
    def row(kind, *fields):
        clean = ("".join(c if 32 <= ord(c) < 127 else " " if c in "\t\n\r" else "?"
                         for c in str(value))[:360] for value in fields)
        print(kind, *clean, sep="\t")

    active = project.active()
    try:
        candidate, proof = project.candidate(active)
        ready = candidate != active["current"]
        row("P", int(ready), int(bool(active["previous"])), candidate,
            active["current"] or "", active["previous"] or "",
            f"{'Ready to publish' if ready else 'Installed'} | {proof['cases']} passing tests"
            f" | candidate {candidate[:12]} | active {(active['current'] or 'none')[:12]}")
    except (OSError, ValueError, TypeError, KeyError) as error:
        candidate, ready = None, False
        row("P", 0, int(bool(active["previous"])), "",
            active["current"] or "", active["previous"] or "",
            f"Active {(active['current'] or 'none')[:12]} | {error}")

    if view == "review":
        import difflib

        row("E", "Review", "Draft compared with the installed source")
        if ready:
            row("E", "Verified", f"{proof['cases']} passing regression and draft tests")
            try:
                tests = cases(read_small(WORK / "versions" / candidate / "regressions.json"))
                for case in tests[:8]:
                    row("E", "Pass", case["name"])
            except (OSError, ValueError, KeyError, TypeError) as error:
                row("E", "Error", error)
        before = (read_small(WORK / "versions" / active["current"] / SOURCE).decode().splitlines()
                  if active["current"] else [])
        after = read_small(SOURCE).decode().splitlines()
        for index, line in enumerate(difflib.unified_diff(before, after, "installed", "draft", lineterm="")):
            if index == 30:
                row("E", "Diff", "[more changes]")
                break
            row("E", "+" if line.startswith("+") else "-" if line.startswith("-") else "Diff", line)
        return

    timeline = None
    for name in filenames:
        try:
            _, events = trace_events(project, name)
        except ValueError:
            continue
        status = events[-1]["data"].get("status", "Working") if events[-1]["event"] == "turn_finished" else "Working"
        row("S", name, status, events[0]["data"]["task"])
        if (not selected and timeline is None) or name == selected:
            timeline = events
    if timeline is None and selected:
        try:
            _, timeline = trace_events(project, selected)
        except ValueError:
            pass
    display = []
    for event in timeline or ():
        kind, data = event["event"], event["data"]
        if kind == "turn_started":
            display.append(("You", data["task"]))
        elif kind == "tool_started":
            display.append(("Running", data.get("label") or f"{data.get('tool', '')} {data.get('target', '')}"))
        elif kind == "tool_finished":
            result = data.get("result", {})
            detail = result.get("detail", "")
            lines = detail.splitlines()
            verdict = next((line for line in reversed(lines)
                            if line.startswith(("CHECK PASSED ", "CHECK REUSED ", "desktop: "))), "")
            if not verdict and len(lines) > 1 and lines[0] == "Command exited with status 0.":
                verdict = lines[1]
            display.append((result.get("status", ""), verdict or detail))
        elif kind == "model_started":
            display.append(("Model", "Waiting for reply"))
        elif kind == "model_finished" and data.get("result", {}).get("status") != "ok":
            display.append(("Model error", data["result"].get("detail", "")))
        elif kind == "assistant":
            display.append(("Agent", data.get("text", "")))
        elif kind == "turn_finished" and data.get("status") != "ok":
            display.append((data.get("status", ""), data.get("detail", "")))
    for kind, detail in display[-24:]:
        row("E", kind, detail)


def main(args):
    match args:
        case ["new", path]:
            create(path)
            return
        case ["read", "guide"]:
            print(read_small(GUIDE).decode(), end="")
            return
    project = Project()
    if 3 <= len(args) <= 15 and args[0] == "panel-info" and args[2] in ("timeline", "review"):
        panel_info(project, args[1], args[2], args[3:])
        return
    if args and args[0] == "resume-info" and len(args) <= 2:
        print(json.dumps(resume_info(project, args[1] if len(args) == 2 else None), ensure_ascii=False))
        return
    files = {"source": SOURCE, "tests": TESTS, "sdk": Path("/src/desktop/window-app.h")}
    match args:
        case ["read", name, *line_range] if name in files and len(line_range) in (0, 2):
            lines = read_small(files[name]).decode().splitlines()
            start, end = map(int, line_range) if line_range else (1, 20)
            if not 1 <= start <= end <= start + 39:
                raise ValueError("read a range of at most 40 lines")
            for index, line in enumerate(lines[start - 1:end], start):
                print(f"{index}: {line}")
        case ["write", name, content] | ["patch", name, content, _] if name in ("source", "tests"):
            path = files[name]
            if args[0] == "patch":
                original = read_small(path).decode()
                if not content or original.count(content) != 1:
                    raise ValueError("patch text must occur exactly once")
                content = original.replace(content, args[3], 1)
            data = content.encode()
            if not data.strip() or len(data) > MAX_TEXT:
                raise ValueError(f"write 1–{MAX_TEXT} bytes")
            if name == "tests":
                cases(data)
            write_atomic(path, data)
            print(f"Saved {path}. Run check.")
        case ["status"]:
            project.status()
        case ["check"]:
            project.check()
        case ["check", "--reuse"]:
            project.check(reuse=True)
        case ["publish" | "rollback"]:
            project.release(args[0])
        case ["publish" | "rollback", expected] if DIGEST.fullmatch(expected):
            project.release(args[0], expected)
        case _:
            raise ValueError("use new PATH, read, write, patch, status, check, publish or rollback")


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print(f"desktop: {error}", file=sys.stderr)
        sys.exit(1)
