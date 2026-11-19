"""Agent front end for the C kernel. Model replies are data, never shell input."""

import json
import os
import stat
import struct
import subprocess
import sys

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


def request(action, *fields, retry=True):
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
            + "\nRegenerate a JSON object with tool and args, with args an array of strings.",
            *rest,
            retry=False,
        )
    if status != "ok":
        fail("model error: " + visible(values[0] if values else status))
    return values


def visible(text):
    return "".join(c if c.isprintable() or c == "\n" else "?" for c in str(text))


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


def run_command(argv, root):
    import codecs

    decoder = codecs.getincrementaldecoder("utf-8")("replace")
    output = []
    used = 0
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
            print(text, end="", flush=True)
            if used < 2048:
                output.append(text[: 2048 - used])
                used += len(output[-1])
        tail = visible(decoder.decode(b"", final=True))
        print(tail, end="", flush=True)
        return child.wait(), "".join(output) + tail


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


def session(root, task, mode, conversation="", bypass=False):
    root = os.path.abspath(root)
    if mode in ("chat", "run"):
        project_root(root, mode == "chat")
    if not os.path.isdir(root) and not (mode == "tune" and not os.path.exists(root)):
        fail("project directory not found")
    system = mode == "system"
    tune = mode == "tune"
    chat = mode == "chat"
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
                )
            except Paused:
                updates += (
                    keyboard_line("agent paused. Enter a follow-up.\nagent> ") + "\n"
                )

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
            except Paused:
                updates += (
                    keyboard_line("agent paused. Enter a follow-up.\nagent> ") + "\n"
                )
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
    return ""


def chat_session(root):
    project_root(root, True)
    if not os.path.isdir(root):
        fail("project directory not found")
    turns = []
    history = []
    bypass = True
    helper("header", b"\1" + root.encode())
    print("LixinOS Agent | Bypass on |", root, "|", settings()[0], "tools/turn")
    print(
        "Bypass auto-approves local workspace commands. Staged saves and terminal handoff require approval. /help /bypass /export /exit"
    )
    while True:
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
        if line == "/help":
            print(
                "Recall inputs with your terminal history. /prompt /clear /bypass /export /exit"
            )
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
            print(read("/src/agent/chat.md").decode())
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
        try:
            answer = session(root, line, "chat", "\n".join(turns[-16:]), bypass)
        except (ValueError, OSError, KeyboardInterrupt, Cancelled) as e:
            print("agent:", visible(e))
            continue
        turns.extend(("User: " + line, "Assistant: " + answer))


def main(args):
    if not args:
        fail("usage: agent inspect, improve, rollback, or SCRIPT TASK")
    mode, *rest = args
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
    elif mode == "chat" and len(rest) <= 1:
        chat_session(os.path.abspath(rest[0] if rest else "."))
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
