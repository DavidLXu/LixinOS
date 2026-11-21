"""User-space Agent worker. The desktop workbench runs on both LixinOS kernels."""

import json
import os
import stat
import struct
import subprocess
import sys
import time

ARCHIVE = "/.agent-system-undo"
MANIFEST = ARCHIVE + "/manifest"
POLICY = "/agent-policy/settings.py"
GIT = "/src/git/README.md"


def fail(message):
    raise ValueError(message)


class Cancelled(Exception):
    pass


class Paused(Exception):
    pass


def keyboard_line(prompt, history=None, choice=False, expand=False):
    print(prompt, end="", flush=True)
    line = bytearray()
    draft = b""
    pos = len(history) if history is not None else 0
    while True:
        ch = os.read(0, 1)
        if ch in (b"", b"\x04", b"\x07", b"\x03"):
            print()
            raise Cancelled("cancelled. Discarded staged changes")
        if ch == b"\x10" and not history and choice:
            raise Paused()
        if ch in (b"\r", b"\n"):
            if not line and not choice:
                continue
            print()
            return line.decode(errors="replace")
        if choice and (ch.lower() in (b"y", b"n") or expand and ch.lower() == b"v"):
            print(ch.decode())
            return ch.decode()
        direction = 0
        if ch == b"\x1b":
            lead = os.read(0, 1)
            if lead in (b"[", b"O"):
                key = os.read(0, 1)
                direction = -1 if key == b"A" else 1 if key == b"B" else 0
        elif history is not None and ch in (b"\x10", b"\x0e"):
            direction = -1 if ch == b"\x10" else 1
        elif ch in (b"\x08", b"\x7f"):
            if line:
                while line and line[-1] & 0xC0 == 0x80:
                    line.pop()
                if line:
                    line.pop()
                print("\b \b", end="", flush=True)
        elif ch >= b" " and len(line) < 1024:
            line += ch
            sys.stdout.buffer.write(ch)
            sys.stdout.flush()
        if direction and history is not None:
            if pos == len(history):
                draft = bytes(line)
            pos = max(0, min(len(history), pos + direction))
            line = bytearray(draft if pos == len(history) else history[pos].encode())
            print(
                "\r\x1b[2K" + prompt + line.decode(errors="replace"), end="", flush=True
            )


def approved(question):
    return keyboard_line(question + " [y/N] ", choice=True).lower() == "y"


def helper(action, data=b""):
    return subprocess.run(["/lib/agent-io", action], input=data, capture_output=True)


def model_response(action, fields):
    body = "\t".join([action, *(s.encode().hex() for s in fields)]).encode()
    reply = helper("model", body)
    if reply.returncode == 125:
        raise Cancelled("cancelled. Discarded staged changes")
    if reply.returncode == 126:
        raise Paused()
    if reply.returncode:
        fail("model bridge unavailable; launch with DEEPSEEK_API_KEY")
    status, *values = reply.stdout.decode("ascii").split("\t")
    values = [bytes.fromhex(v).decode() for v in values]
    return status, values


def request(action, *fields, retry=True, trace=None):
    attempt = 1 if retry else 2
    if trace:
        trace.emit("model_started", attempt=attempt, remaining=int(fields[3]))
        if trace.mode == "desktop":
            trace.require()
    started = time.monotonic_ns()
    try:
        status, values = model_response(action, fields)
    except (Cancelled, Paused, KeyboardInterrupt, OSError, ValueError, subprocess.SubprocessError) as error:
        if trace:
            outcome = ("cancelled" if isinstance(error, (Cancelled, KeyboardInterrupt))
                       else "paused" if isinstance(error, Paused) else "failed")
            trace.emit("model_finished", attempt=attempt, remaining=int(fields[3]),
                       result=dict(status=outcome, duration_ms=elapsed_ms(started),
                                   detail=excerpt(error, 512)))
        raise
    if trace:
        result = dict(status="ok" if status == "ok" else "failed", duration_ms=elapsed_ms(started))
        if status != "ok":
            result["detail"] = excerpt(values[0] if values else status, 512)
        trace.emit("model_finished", attempt=attempt, remaining=int(fields[3]), result=result)
    if (
        status == "error"
        and action == "chat-session"
        and retry
        and values[1:] == ["retryable"]
    ):
        print("agent: retrying the model reply", flush=True)
        instruction, *rest = fields
        return request(
            action,
            instruction
            + ("\nRetry using one declared workbench tool with valid arguments."
               if instruction.startswith("You are the LixinOS desktop app agent.")
               else "\nRegenerate a JSON object with tool and args, with args an array of strings."),
            *rest,
            retry=False,
            trace=trace,
        )
    if status != "ok":
        fail("model error: " + visible(values[0] if values else status))
    return values


def visible(text):
    return "".join(c if c.isprintable() or c == "\n" else "?" for c in str(text))


TRACE_PREFIX = ".agent-trace-"
TRACE_SUFFIX = ".jsonl"


def elapsed_ms(started):
    return (time.monotonic_ns() - started) // 1_000_000


def excerpt(value, limit):
    data = visible(value).encode()
    return data.decode() if len(data) <= limit else data[:limit].decode(errors="ignore") + "\n[truncated]"


def list_traces(root):
    paths = [name for name in os.listdir(root) if name.startswith(TRACE_PREFIX)
             and name.endswith(TRACE_SUFFIX) and os.path.isfile(os.path.join(root, name))]
    if not paths:
        print("agent: no traces in this project")
    for name in sorted(paths, reverse=True)[:8]:
        print(os.path.join(root, name))


def fingerprint(data):
    return "oversize" if len(data) > 32768 else f"{len(data)}:{checksum(data):08x}"


def desktop_checkpoint(root):
    def file_fingerprint(path):
        try:
            with open(path, "rb") as stream:
                return fingerprint(stream.read(32769))
        except (FileNotFoundError, IsADirectoryError):
            return None

    return dict(source=file_fingerprint(os.path.join(root, "app.c")),
                tests=file_fingerprint(os.path.join(root, "tests.json")),
                candidate=file_fingerprint(os.path.join(root, ".desktop/candidate")),
                installed=file_fingerprint(f"/bin/.{os.path.basename(root)}.desktop-release"))


class Trace:
    def __init__(self, root, mode, task, session_id, resumed_from=None, auto_approve=True,
                 continues_task=False):
        self.root = root
        self.mode = mode
        self.session_id = session_id
        self.path = None
        self.started = time.monotonic_ns()
        self.seq = 0
        self.size = 0
        self.disabled = False
        start = dict(project=root, mode=mode, task=excerpt(task, 2048),
                     unix_seconds=int(time.time()), auto_approve=auto_approve)
        if resumed_from is not None:
            start["resumed_from"] = resumed_from
            start["continues_task"] = continues_task
        self.emit("turn_started", **start)

    def open_trace(self):
        if self.path is not None:
            return open(self.path, "r+b", buffering=0)
        stamp = int(time.time())
        for index in range(1, 10000):
            path = os.path.join(self.root, f"{TRACE_PREFIX}{stamp:010d}-{index:04d}{TRACE_SUFFIX}")
            try:
                stream = open(path, "xb", buffering=0)
            except FileExistsError:
                continue
            self.path = path
            return stream
        fail("no available trace filename")

    def emit(self, event, **data):
        if self.disabled:
            return
        try:
            with self.open_trace() as stream:
                turn_id = os.path.basename(self.path)
                if self.session_id is None:
                    self.session_id = turn_id
                record = dict(schema=2, seq=self.seq + 1, elapsed_ms=elapsed_ms(self.started),
                              event=event, session_id=self.session_id, turn_id=turn_id, data=data)
                if self.mode == "desktop" and event in ("turn_started", "tool_started", "tool_finished", "turn_finished"):
                    record["workspace"] = desktop_checkpoint(self.root)

                def encode():
                    return (json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n").encode()

                line = encode()
                # The port commits up to 4 KiB in one write. Keep long previews
                # from leaving half a JSON record on the disk.
                if len(line) > 4096:
                    for key in ("text", "task"):
                        if key in data:
                            data[key] = excerpt(data[key], 768)
                    if "result" in data:
                        result = {key: value for key, value in data["result"].items() if key != "artifact"}
                        if "detail" in result:
                            result["detail"] = excerpt(result["detail"], 768)
                        data["result"] = result
                    data["truncated"] = True
                    line = encode()
                    if len(line) > 4096:
                        fail("trace record exceeds the write limit")
                if self.size + len(line) > 512 * 1024:
                    fail("trace size limit reached")
                stream.seek(self.size)
                if stream.write(line) != len(line):
                    fail("incomplete trace write")
            self.size += len(line)
            self.seq += 1
        except (OSError, ValueError) as error:
            self.disabled = True
            print("agent: trace stopped:", visible(error), flush=True)

    def require(self):
        if self.disabled:
            fail("desktop session log could not be saved")

    def finish(self, status, detail=None):
        result = dict(status=status, duration_ms=elapsed_ms(self.started))
        if detail is not None:
            result["detail"] = excerpt(detail, 512)
        self.emit("turn_finished", **result)


def trace_tool(tool, args, desktop, step):
    action = dict(step=step, call_id=step, tool=excerpt(tool, 32))
    if desktop and desktop_tool(tool, args):
        action["target"] = args[3]
    elif args and tool in ("exec", "terminal", "read", "list", "write", "test", "smoke", "verify"):
        action["target"] = excerpt(args[0], 256)
    return action


def read(path):
    with open(path, "rb") as f:
        return f.read()


def state(path):
    try:
        info = os.stat(path)
    except FileNotFoundError:
        return b"M"
    return b"D" if stat.S_ISDIR(info.st_mode) else b"F" + read(path)


def field(data):
    return struct.pack("<I", len(data)) + data


def checksum(data):
    value = 2166136261
    for b in data:
        value = ((value ^ b) * 16777619) & 0xFFFFFFFF
    return value


def signature(data):
    if data == b"M":
        return "Missing"
    if data == b"D":
        return "Directory"
    return {"File": {"bytes": len(data) - 1, "checksum": checksum(data[1:])}}


class Edit:
    def __init__(self):
        result = helper("versions")
        if result.returncode:
            fail("cannot read guest revision history")
        self.version_bytes = result.stdout
        self.versions = json.loads(self.version_bytes or b"{}")
        self.old = {}
        self.new = {}

    def get(self, path):
        if path not in self.old:
            self.old[path] = state(path)
        return self.new.get(path, self.old[path])

    def set(self, path, data):
        self.get(path)
        self.new[path] = data

    def commit(self):
        data = bytearray(field(self.version_bytes))
        data.extend(
            field(
                json.dumps(self.versions, separators=(",", ":")).encode()
                if self.versions
                else b""
            )
        )
        # Children disappear before their parents; create parents before files.
        paths = sorted(
            self.new,
            key=lambda p: (
                self.new[p] != b"M",
                -p.count("/") if self.new[p] == b"M" else p.count("/"),
                p,
            ),
        )
        for path in paths:
            data.extend(
                field(path.encode()) + field(self.old[path]) + field(self.new[path])
            )
        if helper("apply", data).returncode:
            fail(
                "could not commit; check for conflicting edits or insufficient disk space"
            )


def scoped(root, name, read_proc=False):
    if not name or any(ord(c) < 32 or ord(c) == 127 for c in name):
        fail("invalid path")
    path = os.path.normpath(os.path.join(root, name))
    if (
        len(path.encode()) > 512
        or not path.startswith("/")
        or path.startswith("//")
        or root != "/"
        and path != root
        and not path.startswith(root + "/")
        or ".git" in path.lower().split("/")
        or (not read_proc and (path == "/proc" or path.startswith("/proc/")))
        or path == ARCHIVE
        or path.startswith(ARCHIVE + "/")
    ):
        fail("path outside the permitted workspace")
    return path


def tested(path, source, stdin):
    p = helper(
        "test", field(path.encode()) + field(source.encode()) + field(stdin.encode())
    )
    if p.returncode == 125:
        raise Cancelled("turn cancelled. Discarded staged changes.")
    if p.returncode == 126:
        raise Paused()
    if p.returncode == 123:
        fail("test exceeded the output limit")
    if p.returncode == 124:
        fail("test timed out")
    if p.returncode:
        fail("test failed: " + visible(p.stdout.decode(errors="replace")[:512]))
    return p.stdout.decode().replace("\r\n", "\n")


def suite(path, source, cases):
    if not cases or len(cases) > 12:
        fail("invalid test suite size")
    for case in cases:
        actual = tested(path, source, case["stdin"])
        if actual != case["stdout"]:
            fail("expected %r, got %r" % (case["stdout"], actual[:512]))


def cases(fields):
    if len(fields) % 2 or not 2 <= len(fields) <= 6:
        fail("invalid model tests")
    return [dict(stdin=a, stdout=b) for a, b in zip(fields[::2], fields[1::2])]


def valid_cases(items):
    if not isinstance(items, list) or not 1 <= len(items) <= 12:
        fail("invalid test cases")
    for item in items:
        if not isinstance(item, dict) or set(item) != {"stdin", "stdout"}:
            fail("invalid test case")
        for k, limit in [("stdin", 1024), ("stdout", 2048)]:
            if not isinstance(item[k], str) or len(item[k].encode()) > limit:
                fail("invalid test case")
    if len({c["stdin"] for c in items}) != len(items):
        fail("duplicate test input")
    return items


def script(args, improve=False, tune=False):
    if len(args) < 2 or len(args) % 2:
        fail("expected SCRIPT TASK [--baseline FILE] [--examples FILE]")
    path, task = scoped(os.getcwd(), args[0]), args[1]
    if not path.endswith(".py") or (path == POLICY and not tune):
        fail("invalid script path")
    edit = Edit()
    old = edit.get(path)
    if improve and old[:1] != b"F":
        fail("original script unavailable")
    if not improve and old != b"M":
        fail("agent output already exists")
    source = old[1:].decode()
    opts = {}
    for flag, filename in zip(args[2::2], args[3::2]):
        if flag not in ("--baseline", "--examples") or flag in opts:
            fail("invalid test options")
        opts[flag] = valid_cases(json.loads(read(filename)))
    baseline = []
    if improve:
        baseline = edit.versions.get(path, {}).get("tests") or opts.get("--baseline")
        if not baseline:
            print("agent: no saved tests")
            baseline = cases(request("baseline", source))
        suite(path, source, baseline)
        print("agent: passed", len(baseline), "baseline tests")
    extra = opts.get("--examples") or cases(
        request("examples", task, source) if improve else request("tests", task)
    )
    tests = list(baseline)
    for c in extra:
        if c not in tests:
            tests.append(c)
    valid_cases(tests)
    proposal = feedback = ""
    for _ in range(3):
        answer = request(
            "script",
            task,
            source,
            proposal,
            feedback,
            *(s for c in tests for s in (c["stdin"], c["stdout"])),
        )
        if len(answer) != 1:
            fail("invalid script response")
        proposal = answer[0]
        try:
            if (
                not proposal.strip()
                or "\0" in proposal
                or proposal == source
                or not 0 < len(proposal.encode()) <= 16384
            ):
                fail("invalid candidate length")
            suite(path, proposal, tests)
            break
        except ValueError as error:
            feedback = str(error)
            print("agent:", visible(feedback))
    else:
        fail("exhausted its attempts")
    edit.set(path, b"F" + proposal.encode())
    edit.versions[path] = dict(
        tests=tests, previous=dict(source=source, tests=baseline) if improve else None
    )
    edit.commit()
    print("agent: all", len(tests), "tests passed")
    print("agent: activated" if improve else "agent: saved", path)


def rollback(name):
    edit = Edit()
    path = scoped(os.getcwd(), name)
    old = edit.versions.get(path, {}).get("previous")
    if not old:
        fail("no previous version")
    edit.set(path, b"F" + old["source"].encode())
    edit.versions[path] = dict(tests=old["tests"], previous=None)
    edit.commit()
    print("agent: restored previous version of", path)


def inspect(question):
    cwd = os.getcwd()
    instruction = (
        "You are the LixinOS read-only system inspector. This standalone guest does not run a Linux kernel. "
        "Request queries (system, cpu, memory, disk, filesystem, list, stat) or return a plain-text answer. "
        "Only use measured metadata. Never request file contents."
    )
    observations = []
    for _ in range(7):
        reply = request(
            "inspect-context",
            instruction,
            question,
            cwd,
            str(6 - len(observations)),
            read(GIT).decode(),
            *observations,
        )
        if len(reply) == 2 and reply[0] == "answer":
            if (
                not observations
                or not reply[1].strip()
                or len(reply[1].encode()) > 4096
            ):
                fail("inspection answer requires measured evidence")
            print(visible(reply[1]))
            return
        if (
            not reply
            or reply[0] != "queries"
            or len(reply) % 2 != 1
            or not 1 < len(reply) <= 1 + 2 * (6 - len(observations))
        ):
            fail("invalid inspection query")
        for tool, arg in zip(reply[1::2], reply[2::2]):
            if tool in ("list", "stat"):
                path = os.path.abspath(arg or ".")
                try:
                    size = os.stat(path).st_size
                    text = "path=" + json.dumps(path) + " size_bytes=" + str(size)
                    if tool == "list":
                        for name in sorted(os.listdir(path))[:64]:
                            info = os.stat(os.path.join(path, name))
                            text += (
                                "\n"
                                + (
                                    "directory"
                                    if stat.S_ISDIR(info.st_mode)
                                    else "file"
                                )
                                + " "
                                + json.dumps(name)
                                + " ("
                                + str(info.st_size)
                                + " bytes)"
                            )
                except OSError as e:
                    text = str(e)
            elif tool == "cpu":
                text = subprocess.check_output(["/bin/cpuinfo"], text=True)
            elif tool in ("memory", "disk", "filesystem", "system"):
                text = subprocess.check_output(["/bin/sysinfo"], text=True)
                if tool == "disk":
                    fields = (
                        subprocess.check_output(["/bin/df"], text=True)
                        .splitlines()[-1]
                        .split()
                    )
                    if len(fields) == 4:
                        text += (
                            "\ncapacity_bytes="
                            + fields[1]
                            + "\nused_bytes="
                            + fields[2]
                        )

            else:
                text = "unsupported inspection tool"
            print("agent: checked", json.dumps(tool))
            observations.append(
                "tool="
                + json.dumps(tool)
                + "\nrequested_path="
                + json.dumps(arg)
                + "\n"
                + text[:4096]
            )
    fail("inspection query budget exhausted")


def review(question, edit):
    expanded = False
    while True:
        for path, data in edit.new.items():
            print("+++ proposed", json.dumps(path), signature(data))
            if data[:1] == b"F":
                lines = data[1:16385].decode(errors="replace").splitlines()
                for line in lines if expanded else lines[:3]:
                    print(visible(line))
                if len(data) > 16385 or not expanded and len(lines) > 3:
                    print("[more lines hidden]")
        print("Press v to expand the preview. Ctrl+P adds a follow-up.")
        key = keyboard_line(question + " [y/N] ", choice=True, expand=True).lower()
        if key != "v":
            return key == "y"
        expanded = True


def archive(edit):
    if os.path.exists(ARCHIVE) and not os.path.isfile(MANIFEST):
        fail("system rollback directory is already occupied")
    prior = list(os.listdir(ARCHIVE)) if os.path.isdir(ARCHIVE) else []
    changes = list(edit.new.items())
    manifest = []
    edit.set(ARCHIVE, b"D")
    for i, (path, new) in enumerate(changes):
        before = edit.old[path]
        manifest.append(
            dict(
                path=path,
                before=signature(before),
                after=signature(new),
                version=edit.versions.pop(path, None),
            )
        )
        if before[:1] == b"F":
            edit.set(ARCHIVE + "/" + str(i), before)
    keep = {
        str(i) for i, row in enumerate(manifest) if isinstance(row["before"], dict)
    } | {"manifest"}
    for name in prior:
        if name not in keep:
            edit.set(ARCHIVE + "/" + name, b"M")
    edit.set(MANIFEST, b"F" + json.dumps(manifest, separators=(",", ":")).encode())


def system_rollback():
    edit = Edit()
    payload = edit.get(MANIFEST)
    if payload[:1] != b"F":
        fail("no saved system change")
    rows = json.loads(payload[1:])
    if not isinstance(rows, list) or not 1 <= len(rows) <= 16:
        fail("invalid rollback manifest")
    seen = set()
    for i, entry in enumerate(rows):
        path = scoped("/", entry["path"])
        if path == "/" or path != entry["path"] or path in seen:
            fail("invalid rollback path")
        seen.add(path)
        if signature(edit.get(path)) != entry["after"] or path in edit.versions:
            fail("a file changed after the system update")
        before = entry["before"]
        data = (
            b"M"
            if before == "Missing"
            else b"D"
            if before == "Directory"
            else edit.get(ARCHIVE + "/" + str(i))
        )
        if signature(data) != before:
            fail("system rollback file is missing or damaged")
        edit.set(path, data)
        if entry.get("version"):
            edit.versions[path] = entry["version"]
    for name in os.listdir(ARCHIVE):
        edit.set(ARCHIVE + "/" + name, b"M")
    edit.set(ARCHIVE, b"M")
    edit.commit()
    print("agent: restored the previous system files")


def policy(data):
    import re

    match = re.fullmatch(rb'print\("(\d{1,2}) (\d{1,4})"\)\s*', data)
    if not match:
        fail("stage only settings.py with valid limits")
    steps, peek = map(int, match.groups())
    if not (1 <= steps <= 60 and 64 <= peek <= 2048):
        fail("invalid agent policy limits")
    return steps, peek


def compile_source(data):
    import tempfile

    with tempfile.TemporaryDirectory(dir="/tmp") as tmp:
        path = tmp + "/source.c"
        with open(path, "wb") as f:
            f.write(data)
        result = subprocess.run(
            ["/bin/tcc", path, "-o", tmp + "/program"], capture_output=True
        )
        if result.returncode:
            fail(
                "compiler failed: "
                + visible(result.stdout.decode(errors="replace")[:512])
            )
        return read(tmp + "/program")


def settings():
    try:
        return policy(read(POLICY))
    except (OSError, ValueError):
        return 50, 512


def project_root(root, chat):
    if (
        root == "/"
        or root == "/agent-policy"
        or root.startswith("/agent-policy/")
        or ".git" in root.split("/")
        or chat
        and (root == "/bin" or root.startswith("/bin/"))
    ):
        fail("choose a project directory outside /bin, /agent-policy and .git")


def run_command(argv, root, preview_lines=None):
    import codecs

    decoder = codecs.getincrementaldecoder("utf-8")("replace")
    output = []
    used = 0
    tail = ""
    lines_left = preview_lines
    with subprocess.Popen(
        ["/lib/agent-command", *argv],
        cwd=root,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    ) as child:
        child.stdin.close()
        while block := os.read(child.stdout.fileno(), 4096):
            text = visible(decoder.decode(block))
            if lines_left is None:
                print(text, end="", flush=True)
            elif lines_left > 0:
                preview = "".join(text.splitlines(keepends=True)[:lines_left])
                print(preview, end="", flush=True)
                lines_left -= preview.count("\n")
            if used < 2048:
                output.append(text[: 2048 - used])
                used += len(output[-1])
            if preview_lines is not None:
                tail = (tail + text)[-1024:]
        final = visible(decoder.decode(b"", final=True))
        if lines_left is None or lines_left > 0:
            print(final, end="", flush=True)
        if preview_lines is not None and used == 2048:
            return child.wait(), "".join(output)[:768] + "\n[more output]\n" + tail + final
        return child.wait(), "".join(output) + final


def runner_actions(child, mode, budget, peek, ask):
    """Drive the guest-replaceable /bin/agent-run through its exchange ABI."""
    result = b""
    requested = False
    while True:
        header = child.stdout.read(8)
        if len(header) != 8:
            fail("agent-run exited without submitting a tool")
        operation, size = struct.unpack("<II", header)
        if size > 32768:
            fail("invalid agent-run packet")
        data = child.stdout.read(size)
        if len(data) != size:
            fail("incomplete agent-run packet")
        submission = None
        if operation == 5 and not size:
            response = (mode + str(budget) + " " + str(peek)).encode()
        elif operation in (0, 2) and not requested:
            text = data.decode()
            remaining, instruction = (
                text.split("\0", 1) if operation == 2 else (str(budget), text)
            )
            if not 0 <= int(remaining) <= 60 or "\0" in instruction:
                fail("invalid runner policy")
            fields = ask(instruction, remaining)
            if not fields or any("\0" in f for f in fields):
                fail("invalid model tool")
            response = ("\0".join(fields) + "\0").encode()
            requested = True
        elif operation == 1 and requested and data.endswith(b"\0"):
            submission = data[:-1].decode().split("\0")
            if len(submission) > (65 if submission[0] in ("exec", "terminal") else 4):
                fail("too many tool arguments")
            requested = False
            response = b""
        elif operation == 3 and not size:
            response, result = result, b""
        elif operation == 4 and size <= 4096:
            print(visible(data.decode()), end="", flush=True)
            response = b""
        else:
            fail("unexpected agent-run operation")
        if len(response) > 32768:
            fail("agent-run response exceeds mailbox")
        child.stdin.write(field(response))
        child.stdin.flush()
        if submission is not None:
            result = yield submission


DESKTOP_SCRIPT = "/src/agent/desktop.py"
DESKTOP_GUIDE = "/src/agent/desktop.md"


def desktop_tool(tool, args):
    if tool != "exec" or args[:3] != ["python3", "-I", DESKTOP_SCRIPT] or len(args) < 4:
        return False
    return len(args) - 4 in {"read": (1, 3), "write": (2,), "patch": (3,),
                             "check": (0,), "status": (0,)}.get(args[3], ())


def named_desktop_tool(tool, args):
    operation = {
        ("read_file", 1): "read", ("read_file", 3): "read",
        ("write_file", 2): "write", ("apply_patch", 3): "patch",
        ("check_app", 0): "check", ("app_status", 0): "status",
    }.get((tool, len(args)))
    return ("exec", ["python3", "-I", DESKTOP_SCRIPT, operation, *args]) if operation else (tool, args)


def desktop_intent(root, args):
    operation, target, *content = args[3:]
    if operation not in ("write", "patch") or target not in ("source", "tests"):
        return None
    path = os.path.join(root, "app.c" if target == "source" else "tests.json")
    before = read(path)
    if operation == "write":
        after = content[0].encode()
    else:
        old, new = (text.encode() for text in content)
        if not old or before.count(old) != 1:
            return None
        after = before.replace(old, new, 1)
    if not after.strip() or len(after) > 32768:
        return None
    return dict(file=target, before=fingerprint(before), after=fingerprint(after))


def desktop_observations(observations, trace, compacted):
    if len(observations) <= 12 and sum(len(item) for item in observations) <= 16384:
        return observations, compacted
    older, recent = observations[:-8], observations[-8:]
    if not older:
        return recent, compacted
    entries = [json.loads(item) for item in older]
    checks = [entry for entry in entries if entry["operation"] == "check"]
    summary = dict(tool="history_summary", status="ok", detail="Earlier tool results are saved in the session log.",
                   count=len(older), calls=[(e["operation"], e["status"]) for e in entries[-8:]])
    if checks:
        summary["last_check"] = excerpt(checks[-1]["detail"], 512)
    if compacted != len(older):
        trace.emit("context_compacted", omitted=len(older), retained=len(recent))
    return [json.dumps(summary), *recent], len(older)


def desktop_session(root, task, conversation, bypass, trace):
    budget, peek = settings()
    instruction = read(DESKTOP_GUIDE).decode()
    git = read(GIT).decode()
    observations = []
    updates = ""
    checked_at = compacted = None

    def follow_up():
        nonlocal updates
        note = keyboard_line("agent paused. Enter a follow-up.\nagent> ")
        trace.emit("follow_up", text=excerpt(note, 1024))
        trace.require()
        updates = excerpt(updates + note + "\n", 4096)

    def finish(summary):
        summary = excerpt(summary, 4096)
        print("\x1b[95m" + summary + "\x1b[0m", flush=True)
        trace.emit("assistant", text=summary)
        trace.require()
        return summary

    for step in range(1, budget + 2):
        while True:
            try:
                context, compacted = desktop_observations(observations, trace, compacted)
                tool, *args = request("chat-session", instruction, task, root,
                                      str(budget + 1 - step), updates, "", git,
                                      conversation, *context, trace=trace)
                break
            except Paused:
                follow_up()
        if tool == "finish" and len(args) == 1:
            return finish(args[0])
        if step > budget:
            break
        tool, args = named_desktop_tool(tool, args)
        allowed = desktop_tool(tool, args)
        operation = args[3] if allowed else ""
        target = " ".join(args[4:7]) if operation == "read" else args[4] if operation in ("write", "patch") else ""
        title = f"{operation} {target}".strip() if allowed else tool
        print(f"\x1b[36m [{step:02}/{budget}] {excerpt(title, 180)}\x1b[0m", flush=True)
        action = trace_tool(tool, args, True, step)
        action["label"] = excerpt(title, 180)
        rc, output = None, ""
        reuse = operation == "check" and checked_at == updates
        started = time.monotonic_ns()
        try:
            intent = desktop_intent(root, args) if operation in ("write", "patch") else None
            trace.emit("tool_started", **action, **({"intent": intent} if intent else {}))
            trace.require()
            if not allowed:
                status, output = "failed", "desktop mode accepts only workbench tools. See /prompt"
            elif not bypass and not approved(f"Run {title} in {root}?"):
                status, output = "declined", "Operator declined the command."
            else:
                argv = ["/bin/python3", *args[1:]]
                if reuse:
                    argv.append("--reuse")
                rc, output = run_command(argv, root, preview_lines=3)
                if rc == 125:
                    raise Cancelled("turn cancelled")
                if rc == 126:
                    raise Paused()
                status = "timed_out" if rc == 124 else "ok" if rc == 0 else "failed"
        except Paused:
            follow_up()
            status, output = "paused", "Operator paused the command. Inspect the draft before another edit."
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            status, output = "failed", visible(error)
        detail = f"Command exited with status {rc}.\n{output}" if rc is not None else output
        result = dict(status=status, detail=excerpt(detail, 2600))
        if rc is not None and rc not in (124, 125, 126):
            result["exit_code"] = rc
        trace.emit("tool_finished", **action, duration_ms=elapsed_ms(started), result=result)
        trace.require()
        preview = excerpt(detail, peek)
        if operation == "check" and rc == 0:
            proof = next((line for line in reversed(output.splitlines()) if line.startswith(("CHECK PASSED ", "CHECK REUSED "))), "")
            if proof and proof not in preview:
                preview += "\n" + proof
        observations.append(json.dumps(dict(tool=tool, operation=operation, label=title,
                                            **{**result, "detail": preview}), ensure_ascii=False))
        if operation == "check":
            checked_at = updates if status == "ok" else None
            if reuse and status == "ok" and output.startswith("CHECK REUSED "):
                return finish("Stopped after a repeated check. The unchanged draft has a passing candidate for /publish.")
    print("agent: tool budget exhausted")
    return None


def session(root, task, mode, conversation="", bypass=False, trace=None):
    root = os.path.abspath(root)
    desktop = mode == "desktop"
    if mode in ("chat", "desktop", "run"):
        project_root(root, mode != "run")
    if not os.path.isdir(root) and not (mode == "tune" and not os.path.exists(root)):
        fail("project directory not found")
    if desktop:
        return desktop_session(root, task, conversation, bypass, trace)
    system = mode == "system"
    tune = mode == "tune"
    chat = mode in ("chat", "desktop")
    budget, peek = settings()
    if tune:
        budget = 12
    chat_instruction = read("/src/agent/chat.md").decode() if chat else ""
    git_reference = read(GIT).decode()
    edit = Edit()
    checks = {}
    observations = []
    evidence = {}
    updates = ""
    verified = {}
    platform = "The guest is standalone LixinOS with a C/assembly kernel built by TCC. It does not run a Linux kernel."
    if chat:
        platform += " Model commands run locally in the workspace. The operator can open an interactive Bash, Python or editor terminal."

    def apply():
        if not edit.new:
            return "nothing staged"
        if not system and any(not checks.get(p) for p in edit.new):
            fail("every staged script must pass its test before review")
        if not review(
            "Apply the staged "
            + ("system changes" if system else "scripts")
            + " to disk?",
            edit,
        ):
            edit.new.clear()
            checks.clear()
            return "discarded staged changes"
        if system:
            archive(edit)
        else:
            for path in edit.new:
                before = edit.old[path]
                if tune and before == b"M":
                    before = b'Fprint("50 512")\n'
                history = edit.versions.get(path, {}).get("tests", [])
                if tune:
                    history = [dict(stdin="", stdout="%d %d\n" % policy(before[1:]))]
                edit.versions[path] = dict(
                    tests=checks[path],
                    previous=(
                        dict(source=before[1:].decode(), tests=history or checks[path])
                        if before[:1] == b"F"
                        else None
                    ),
                )
        if tune and not os.path.isdir(root):
            edit.set(root, b"D")
        edit.commit()
        return "applied staged " + ("system changes" if system else "scripts")

    def ask(instruction, remaining):
        nonlocal updates
        while True:
            try:
                return request(
                    "chat-session" if chat else "project-session-context",
                    (chat_instruction if chat else instruction) + "\n\n" + platform,
                    task,
                    root,
                    remaining,
                    updates,
                    "staged: " + ", ".join(edit.new),
                    git_reference,
                    *([conversation] if chat else []),
                    *observations,
                    trace=trace,
                )
            except Paused:
                note = keyboard_line("agent paused. Enter a follow-up.\nagent> ")
                updates += note + "\n"
                if trace and note:
                    trace.emit("follow_up", text=excerpt(note, 512))

    print("agent: starting /bin/agent-run", flush=True)
    child = subprocess.Popen(
        ["/bin/agent-run"], stdin=subprocess.PIPE, stdout=subprocess.PIPE
    )
    actions = runner_actions(
        child, "s" if system else "t" if tune else "p", budget, peek, ask
    )
    delivery = None
    try:
        for step in range(budget + 1):
            tool, *args = actions.send(delivery)
            if step == budget and tool != "finish":
                break
            status = "ok"
            record = {}
            action = trace_tool(tool, args, False, step + 1) if trace else None
            if trace and tool != "finish":
                trace.emit("tool_started", **action)
            started = time.monotonic_ns()
            try:
                if tool == "finish" and len(args) == 1:
                    if chat and any(state(p) != v for p, v in verified.items()):
                        fail(
                            "a verified file changed on disk; verify the current file before finishing"
                        )
                    summary = visible(args[0])
                    if edit.new:
                        if chat and any(not checks.get(p) for p in edit.new):
                            summary = "The staged scripts did not pass their tests. Discarded staged changes."
                        else:
                            applied = apply()
                            print("agent:", applied)
                            if applied.startswith("discarded"):
                                summary = "The staged changes were discarded."
                    print("\x1b[95m" + summary + "\x1b[0m" if chat else summary)
                    if trace:
                        trace.emit("assistant", text=excerpt(summary, 4096))
                    return summary
                if tool == "apply" and not args:
                    result = apply()
                    edit = Edit()
                    checks = {}
                    status = "saved"
                elif chat and tool in ("exec", "terminal") and args:
                    name = args[0]
                    program = (
                        os.path.join("/bin", name)
                        if "/" not in name
                        else os.path.abspath(os.path.join(root, name))
                    )
                    argv = [program, *args[1:]]
                    if tool == "terminal":
                        # The operator owns terminal input. Do not pass a model-supplied
                        # shell command, startup script, or SSH remote command.
                        if (
                            program in ("/bin/bash", "/bin/python", "/bin/python3")
                            and len(args) == 1
                        ):
                            argv = [program]
                        elif program in ("/bin/nano", "/bin/kilo") and len(args) == 2:
                            argv = [program, scoped(root, args[1])]
                        else:
                            fail(
                                "use an interactive shell, interpreter or local editor"
                            )
                        if not approved(
                            "Hand terminal control to "
                            + json.dumps(argv)
                            + " in "
                            + root
                            + "?"
                        ):
                            fail("operator declined")
                        proc = subprocess.run(argv, cwd=root)
                        output = "terminal returned"
                        rc = proc.returncode
                    else:
                        if not bypass and not approved(
                            "Run " + json.dumps(argv) + " in " + root + "?"
                        ):
                            fail("operator declined")
                        rc, output = run_command(argv, root)
                        if rc == 125:
                            raise Cancelled("turn cancelled")
                        if rc == 126:
                            raise Paused()
                    status = "timed_out" if rc == 124 else "ok" if not rc else "failed"
                    if rc != 124:
                        record["exit_code"] = rc
                    record["command"] = json.dumps(argv)
                    result = "status " + str(rc) + " " + output[:peek]
                elif chat and tool == "verify" and 1 <= len(args) <= 3:
                    path = scoped(root, args[0])
                    record["path"] = path
                    data = state(path) if path not in edit.new else edit.new[path]
                    if path in edit.new:
                        status = "staged"
                        result = "file is staged; apply before verifying"
                    elif data[:1] != b"F":
                        fail("guest file not found")
                    else:
                        exact, smoke, failed = evidence.get((path, data), [0, 0, 0])
                        info = subprocess.check_output(["/bin/sysinfo"], text=True)
                        generation = int(
                            next(
                                v.split("=")[1]
                                for v in info.splitlines()
                                if v.startswith("committed_snapshot_generation=")
                            )
                        )
                        artifact = dict(
                            bytes=len(data) - 1,
                            snapshot=generation,
                            checks=dict(
                                exact_passed=exact, smoke_passed=smoke, failed=failed
                            ),
                        )
                        if len(args) > 1:
                            artifact["launch"] = args[1]
                        if len(args) > 2:
                            artifact["interactive"] = (
                                "scripted" if smoke else "unchecked"
                            )
                        verified[path] = data
                        record["artifact"] = artifact
                        status = "verified"
                        result = "Verified " + path + " on disk"
                elif tool == "system" and not args:
                    result = subprocess.check_output(["/bin/sysinfo"], text=True)
                elif tool == "list" and len(args) == 1:
                    path = scoped(root, args[0], system)
                    entries = set(os.listdir(path)) if os.path.isdir(path) else set()
                    if (
                        not os.path.isdir(path)
                        and edit.new.get(path) != b"D"
                        and not (tune and path == root)
                    ):
                        fail("directory not found")
                    for changed, data in edit.new.items():
                        if os.path.dirname(changed) == path:
                            n = os.path.basename(changed)
                            if data == b"M":
                                entries.discard(n)
                            else:
                                entries.add(n)
                    if tune and path == root:
                        entries.add("settings.py")
                    names = sorted(entries)
                    result = "\n".join(
                        (
                            "directory"
                            if os.path.isdir(os.path.join(path, n))
                            else "file"
                        )
                        + " "
                        + json.dumps(n)
                        for n in names[:100]
                    )
                elif tool in ("read", "readhex") and 1 <= len(args) <= 2:
                    path = scoped(root, args[0], system)
                    data = edit.get(path)
                    if tune and path == POLICY and data == b"M":
                        data = b'Fprint("50 512")\n'
                    if data[:1] != b"F":
                        fail("guest file not found")
                    start = int(args[1]) if len(args) == 2 else 0
                    if start < 0:
                        fail("invalid offset")
                    result = (
                        data[1 + start : 1 + start + peek].hex()
                        if tool == "readhex"
                        else data[1 + start : 1 + start + peek].decode(errors="replace")
                    )
                elif tool in ("test", "smoke") and len(args) == 3:
                    path = scoped(root, args[0])
                    data = edit.get(path)
                    if data[:1] != b"F":
                        fail("script not found")
                    output = tested(path, data[1:].decode(), args[1])
                    if output != args[2] if tool == "test" else args[2] not in output:
                        fail(
                            "test failed: expected "
                            + repr(args[2])
                            + ", got "
                            + repr(output[:peek])
                        )
                    case = dict(stdin=args[1], stdout=output)
                    previous = checks.setdefault(path, [])
                    if case not in previous:
                        valid_cases(previous + [case])
                        previous.append(case)
                    evidence.setdefault((path, data), [0, 0, 0])[
                        int(tool == "smoke")
                    ] += 1
                    record.update(path=path, exit_code=0)
                    status = "test_passed"
                    result = "test passed"
                elif (
                    tool
                    in (
                        "write",
                        "append",
                        "replace",
                        "writehex",
                        "appendhex",
                        "mkdir",
                        "remove",
                        "copy",
                        "compile",
                    )
                    and args
                ):
                    path = scoped(root, args[0])
                    old = edit.get(path)
                    if path == root or (
                        not system and (tool != "write" or not path.endswith(".py"))
                    ):
                        fail("path outside the allowed project files")
                    if tune and path != POLICY:
                        fail("stage only settings.py")
                    if not tune and not system and path == POLICY:
                        fail("use agent tune")
                    if path not in edit.new and len(edit.new) >= (16 if system else 4):
                        fail("too many staged files")
                    if tool in ("mkdir", "remove") and len(args) == 1:
                        if tool == "mkdir" and old != b"M":
                            fail("path already exists")
                        if tool == "remove" and old == b"M":
                            fail("path not found")
                        new = b"D" if tool == "mkdir" else b"M"
                    elif tool == "replace" and len(args) == 3:
                        if (
                            old[:1] != b"F"
                            or not args[1]
                            or old[1:].count(args[1].encode()) != 1
                        ):
                            fail("old text must occur exactly once")
                        new = b"F" + old[1:].replace(args[1].encode(), args[2].encode())
                    elif tool in ("copy", "compile") and len(args) == 2:
                        source = scoped(root, args[1])
                        data = edit.get(source)
                        if data[:1] != b"F" or source == path:
                            fail("invalid source")
                        new = (
                            data if tool == "copy" else b"F" + compile_source(data[1:])
                        )
                    elif len(args) == 2 and tool in (
                        "write",
                        "append",
                        "writehex",
                        "appendhex",
                    ):
                        data = (
                            bytes.fromhex(args[1])
                            if tool.endswith("hex")
                            else args[1].encode()
                        )
                        new = (
                            old
                            if tool.startswith("append") and old[:1] == b"F"
                            else b"F"
                        ) + data
                    else:
                        fail("invalid tool arguments")
                    if not system and (len(new) > 4097 or b"\x00" in new):
                        fail("invalid staged script")
                    if tune:
                        policy(new[1:])
                    edit.set(path, new)
                    record["path"] = path
                    checks.pop(path, None)
                    status = "staged"
                    result = "staged " + tool + " " + path + " using " + tool
                else:
                    fail("unsupported tool or invalid arguments")
            except (Cancelled, KeyboardInterrupt) as error:
                if trace and tool != "finish":
                    trace.emit("tool_finished", **action, duration_ms=elapsed_ms(started),
                               result=dict(status="cancelled", detail=excerpt(error, 512)))
                raise
            except Paused:
                note = keyboard_line("agent paused. Enter a follow-up.\nagent> ")
                updates += note + "\n"
                if trace and note:
                    trace.emit("follow_up", text=excerpt(note, 512))
                status = "paused"
                result = "Operator paused before completion; review the follow-up."
            except (OSError, ValueError, UnicodeError, subprocess.SubprocessError) as e:
                result = visible(e)
                status = (
                    "timed_out"
                    if "timed out" in result
                    else "declined"
                    if "declined" in result
                    else "needs_tests"
                    if "must pass its test" in result
                    else "test_failed"
                    if tool in ("test", "smoke")
                    else "failed"
                )
                if (
                    tool in ("test", "smoke")
                    and len(args) == 3
                    and "data" in locals()
                    and "path" in locals()
                ):
                    evidence.setdefault((path, data), [0, 0, 0])[2] += 1
            if trace and tool != "finish":
                trace_result = {key: value for key, value in record.items() if key != "command"}
                trace.emit("tool_finished", **action, duration_ms=elapsed_ms(started),
                           result=dict(status=status, detail=excerpt(result, 4096), **trace_result))
            delivery = (
                ("+" if status not in ("failed", "timed_out") else "-")
                + visible(result[:peek])
            ).encode()
            line = "tool " + json.dumps(tool) + ": " + result[:peek]
            if chat:
                line = json.dumps(
                    dict(tool=tool, status=status, detail=result[:peek], **record)
                )
            observations.append(line)
    finally:
        child.stdin.close()
        child.stdout.close()
        child.wait()
    print("agent: tool budget exhausted; discarded staged changes")
    return None


def chat_session(root, desktop=False, resume=None):
    project_root(root, True)
    if not os.path.isdir(root):
        fail("project directory not found")
    turns = []
    history = []
    bypass = resume["auto_approve"] if resume else True
    session_id = resume["session_id"] if resume else None
    resumed_from = resume["from_turn"] if resume else None
    pending_task = resume["task"] if resume and resume["continue_turn"] else None
    helper("header", bytes([1 if bypass else 2]) + root.encode())
    print("LixinOS Agent | Bypass " + ("on" if bypass else "off") + " |", root, "|", settings()[0], "tools/turn")
    if desktop:
        print("Desktop workbench | /status /publish /rollback /help /exit")
    else:
        print("Bypass auto-approves local workspace commands. Staged saves and terminal handoff require approval. /help /bypass /export /exit")
    if resume:
        print("agent:", visible(resume["summary"]))
        turns.append(resume["context"])
    while True:
        continuing = pending_task is not None
        if pending_task is not None:
            line, pending_task = pending_task, None
        else:
            try:
                line = keyboard_line("\x1b[96magent> \x1b[0m", history).strip()
            except (Cancelled, EOFError, KeyboardInterrupt):
                return
        if line and (not history or line != history[-1]):
            history.append(line)
            history[:] = history[-64:]
        if line in ("/exit", "/quit"):
            return
        if not line:
            continue
        if line == "/trace":
            list_traces(root)
            continue
        if desktop and line in ("/status", "/publish", "/rollback"):
            trace = Trace(root, "desktop", line, session_id, auto_approve=bypass)
            session_id = trace.session_id
            action = dict(step=1, call_id=1, tool="desktop_operator", target=line[1:])
            trace.emit("tool_started", **action)
            started = time.monotonic_ns()
            try:
                code = subprocess.run(["/bin/python3", "-I", DESKTOP_SCRIPT, line[1:]], cwd=root).returncode
                result = dict(exit_code=code, status="ok" if code == 0 else "failed",
                              detail="desktop operation completed" if code == 0 else "desktop operation failed")
            except (OSError, KeyboardInterrupt, subprocess.SubprocessError) as error:
                result = dict(status="cancelled" if isinstance(error, KeyboardInterrupt) else "failed",
                              detail=excerpt(error, 512))
                print("agent:", visible(error))
            trace.emit("tool_finished", **action, duration_ms=elapsed_ms(started), result=result)
            trace.finish(result["status"], result["detail"] if result["status"] != "ok" else None)
            continue
        if line == "/help":
            if desktop:
                print("Describe the app or change. /status checks the candidate. /publish installs it. /rollback restores its predecessor. /prompt /export /trace /bypass /exit")
            else:
                print("Recall inputs with your terminal history. /prompt /clear /bypass /export /trace /exit")
            continue
        if line == "/bypass":
            bypass = not bypass
            helper("header", bytes([1 if bypass else 2]) + root.encode())
            print("Bypass on" if bypass else "Bypass off")
            continue
        if line == "/clear":
            turns.clear()
            continue
        if line == "/prompt":
            print(read(DESKTOP_GUIDE if desktop else "/src/agent/chat.md").decode())
            continue
        if line.startswith("/export"):
            if not turns:
                print("agent: no conversation to export yet")
                continue
            try:
                args = line.split(maxsplit=1)
                if len(args) == 2:
                    if "/" in args[1]:
                        fail("use a filename in the project directory")
                    path = scoped(root, args[1])
                    if os.path.exists(path):
                        fail("export file already exists")
                else:
                    path = scoped(root, "agent-conversation.txt")
                    index = 1
                    while os.path.exists(path):
                        index += 1
                        path = scoped(root, "agent-conversation-%d.txt" % index)
                with open(path, "x") as f:
                    f.write("\n".join(turns) + "\n")
                print("agent: exported conversation to", path)
            except (OSError, ValueError) as e:
                print("agent:", visible(e))
            continue
        mode = "desktop" if desktop else "chat"
        context = "\n".join(turns[-16:])
        if desktop:
            prior = resume if continuing else load_desktop_resume(root, optional=True)
            session_id = prior["session_id"] if prior else session_id
            resumed_from = prior["from_turn"] if prior else None
            context = prior["context"] if prior else ""
        trace = Trace(root, mode, line, session_id, resumed_from, bypass, continues_task=continuing)
        session_id = trace.session_id
        resumed_from = None
        try:
            answer = session(root, line, mode, context, bypass, trace)
        except (ValueError, OSError, KeyboardInterrupt, Cancelled, subprocess.SubprocessError) as error:
            trace.finish("cancelled" if isinstance(error, (Cancelled, KeyboardInterrupt)) else "failed", error)
            print("agent:", visible(error))
            continue
        if answer is None:
            trace.finish("tool_limit", "project agent reached its tool limit")
        else:
            trace.finish("ok")
            turns.extend(("User: " + line, "Assistant: " + answer))


def load_desktop_resume(root, trace_file="", optional=False):
    path = [trace_file] if trace_file else []
    result = subprocess.run(["/bin/python3", "-I", DESKTOP_SCRIPT, "resume-info", *path],
                            cwd=root, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL if optional else None, check=not optional)
    return json.loads(result.stdout) if result.returncode == 0 else None


def desktop_work(root, operation, *args):
    root = os.path.abspath(root)
    project_root(root, True)
    subprocess.run(["/bin/python3", "-I", DESKTOP_SCRIPT, "status"], cwd=root, check=True)
    if operation in ("publish", "rollback") and len(args) == 1:
        trace = Trace(root, "desktop", "/" + operation, None)
        action = dict(step=1, call_id=1, tool="desktop_operator", target=operation)
        trace.emit("tool_started", **action)
        started = time.monotonic_ns()
        code = subprocess.run(["/bin/python3", "-I", DESKTOP_SCRIPT, operation, args[0]], cwd=root).returncode
        status = "ok" if code == 0 else "failed"
        trace.emit("tool_finished", **action, duration_ms=elapsed_ms(started),
                   result=dict(status=status, exit_code=code))
        trace.finish(status)
        return code
    if operation == "resume" and len(args) == 1:
        prior = load_desktop_resume(root, args[0])
        if not prior["continue_turn"]:
            return 0
        task = prior["task"]
    elif operation == "send" and len(args) == 2 and 0 < len(args[0].strip().encode()) <= 2048:
        task = args[0]
        prior = load_desktop_resume(root, args[1], optional=not args[1])
    else:
        fail("invalid desktop task request")
    if prior and not prior["auto_approve"]:
        fail("resume this approval-required turn in the terminal")
    trace = Trace(root, "desktop", task, prior["session_id"] if prior else None,
                  prior["from_turn"] if prior else None, continues_task=operation == "resume")
    try:
        answer = session(root, task, "desktop", prior["context"] if prior else "", True, trace)
    except (ValueError, OSError, KeyboardInterrupt, Cancelled, subprocess.SubprocessError) as error:
        trace.finish("cancelled" if isinstance(error, (Cancelled, KeyboardInterrupt)) else "failed", error)
        raise
    if answer is None:
        trace.finish("tool_limit", "project agent reached its tool limit")
        return 1
    trace.finish("ok")
    return 0


def main(args):
    if not args:
        fail("usage: agent inspect, improve, rollback, or SCRIPT TASK")
    mode, *rest = args
    if mode in ("chat", "desktop") and rest[:1] == ["--auto-approve"]:
        rest = rest[1:]
    if mode == "inspect" and len(rest) == 1:
        inspect(rest[0])
    elif mode == "rollback" and len(rest) == 1:
        rollback(rest[0])
    elif mode == "system-rollback" and not rest:
        system_rollback()
    elif mode == "system" and len(rest) == 1:
        session("/", rest[0], "system")
    elif mode == "run" and len(rest) == 2:
        session(rest[0], rest[1], "run")
    elif mode == "tune" and len(rest) == 1:
        session("/agent-policy", rest[0], "tune")
    elif mode == "rollback-tune" and not rest:
        rollback(POLICY)
    elif mode == "desktop" and len(rest) == 2 and rest[0] == "new":
        sys.exit(subprocess.run(["/bin/python3", "-I", DESKTOP_SCRIPT, *rest]).returncode)
    elif mode in ("chat", "desktop") and len(rest) <= 1:
        chat_session(os.path.abspath(rest[0] if rest else "."), mode == "desktop")
    elif mode == "desktop-work" and len(rest) == 3 and rest[:2] == ["/", "store-ideas"]:
        from runpy import run_path
        run_path("/lib/app-store/ideas.py")["store_ideas"](rest[2], request)
    elif mode == "desktop-work" and len(rest) >= 2:
        sys.exit(desktop_work(*rest))
    elif mode == "resume":
        if len(rest) > 2:
            fail("usage: agent resume [PROJECT [TRACE_FILE]]")
        root = os.path.abspath(rest[0] if rest else ".")
        chat_session(root, True, load_desktop_resume(root, rest[1] if len(rest) > 1 else ""))
    elif mode == "trace" and len(rest) <= 1:
        list_traces(os.path.abspath(rest[0] if rest else "."))
    elif mode == "improve":
        script(rest, True)
    else:
        script(args)


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except (
        OSError,
        ValueError,
        KeyError,
        UnicodeError,
        subprocess.SubprocessError,
        Cancelled,
        Paused,
    ) as error:
        print("agent:", visible(error), file=sys.stderr)
        sys.exit(1)
