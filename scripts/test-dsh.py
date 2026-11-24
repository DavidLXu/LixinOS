"""Install DSH on a disposable disk and exercise its public CLI in QEMU."""
import argparse
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import re
from pathlib import Path
import runpy
import secrets
import shutil
import subprocess
import tempfile
import threading

from disk_image import require_local

root = Path(__file__).resolve().parents[1]
native = runpy.run_path(str(root / "scripts/test-native-kernel.py"))
install = runpy.run_path(str(root / "User/gcc/install.py"))["install"]


@contextmanager
def messages_fixture(witness):
    """A local model endpoint. Its second reply requires a real guest tool result."""
    class Endpoint(BaseHTTPRequestHandler):
        def do_POST(self):
            if self.path != "/anthropic/v1/messages":
                self.send_error(404)
                return
            request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            blocks = [block for message in request["messages"]
                      if isinstance(message["content"], list) for block in message["content"]]
            has_tools = any(tool["name"] == "read" for tool in request.get("tools", []))
            results = {b["tool_use_id"]: b for b in blocks if b["type"] == "tool_result"}
            resume = any(b.get("text") == "Resume and run project test." for b in blocks)
            action = None
            answer = "File read test"
            if has_tools:
                steps = [
                    ("read_1", "read", {"file_path": "witness.txt"}, witness),
                    ("write_1", "write", {"file_path": "result.txt", "content": "old"}, "Created file"),
                    ("read_2", "read", {"file_path": "result.txt"}, "old"),
                    ("edit_1", "edit", {"file_path": "result.txt", "old_string": "old",
                                         "new_string": witness}, "updated successfully"),
                ]
                if resume:
                    previous = any(m["role"] == "assistant" and "ready-" + witness in str(m["content"])
                                   for m in request["messages"])
                    if not previous or not all(step[0] in results for step in steps):
                        self.send_error(400, "missing first turn or tool history")
                        return
                    steps.append(("shell_1", "bash", {"command": "cp result.txt output.txt && cat output.txt",
                                  "description": "Run the project copy test"}, witness))
                for call_id, name, args, evidence in steps:
                    if call_id not in results:
                        action = (call_id, name, args)
                        break
                    if results[call_id].get("is_error") or evidence not in str(results[call_id]["content"]):
                        self.send_error(400, "unexpected tool result: " + call_id)
                        return
                if action is None:
                    answer = ("passed-" if resume else "ready-") + witness
                    if resume:
                        verified.set()
            if action:
                call_id, name, args = action
                block = {"type": "tool_use", "id": call_id, "name": name, "input": {}}
                delta = {"type": "input_json_delta", "partial_json": json.dumps(args)}
            else:
                block = {"type": "text", "text": ""}
                delta = {"type": "text_delta", "text": answer}
            events = [
                {"type": "message_start", "message": {"id": "fixture", "type": "message", "role": "assistant",
                 "model": request["model"], "content": [], "usage": {"input_tokens": 10, "output_tokens": 0}}},
                {"type": "content_block_start", "index": 0, "content_block": block},
                {"type": "content_block_delta", "index": 0, "delta": delta},
                {"type": "content_block_stop", "index": 0},
                {"type": "message_delta", "delta": {"stop_reason": "tool_use" if action else "end_turn"},
                 "usage": {"output_tokens": 6}},
                {"type": "message_stop"},
            ]
            body = "".join(f"event: {e['type']}\ndata: {json.dumps(e)}\n\n" for e in events).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, fmt, *args):
            pass

    verified = threading.Event()
    with ThreadingHTTPServer(("127.0.0.1", 0), Endpoint) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            yield server.server_port, verified
        finally:
            server.shutdown()
            thread.join()


def check_headless(serial):
    command = native["command"]
    witness = "guest-" + secrets.token_hex(12)
    directory = "/dsh-" + secrets.token_hex(4)
    command(serial, "mkdir " + directory)
    prompt = f"lixin:{directory}$ ".encode()
    serial.expect(("cd " + directory + "\r").encode(), prompt)
    def run(line, expected=b""):
        command(serial, line, expected=expected, prompt=prompt)
    with messages_fixture(witness) as (port, verified):
        run(f"export HOME={directory} DSH_HOME={directory}/.dsh PATH=/bin")
        run("export DSH_TELEMETRY_DISABLED=1 DEEPSEEK_API_KEY=fixture-key")
        run(f"export DEEPSEEK_BASE_URL=http://10.0.2.2:{port}/anthropic")
        run("echo " + witness + " > witness.txt")
        run("dsh headless --help", b"Answer one task and exit")
        serial.send(b"dsh headless --json 'Read witness.txt and update result.txt.'\r")
        first = serial.until(prompt, timeout=210)
        events = [json.loads(line) for line in first.splitlines() if line.startswith(b"{")]
        assert any(e.get("text") == "ready-" + witness for e in events if e["type"] == "final"), first
        sid = next(e["sessionId"] for e in events if e["type"] == "session")
        assert re.fullmatch(r"session-[a-zA-Z0-9-]+", sid)
        run(f"dsh headless --session-id {sid} 'Resume and run project test.'", ("passed-" + witness).encode())
        run("cat output.txt", witness.encode())
        assert verified.is_set(), "expected the resumed shell result at the Messages endpoint"
    serial.expect(b"cd /\r", native["shell"])


def check(args):
    seed = args.disk.resolve(strict=True)
    require_local(seed, seed.stat())
    with tempfile.TemporaryDirectory(prefix="lixin-dsh-") as temporary:
        disk = Path(temporary) / "test.img"
        if subprocess.run(["cp", "-c", seed, disk], capture_output=True).returncode:
            shutil.copyfile(seed, disk)
        install(args.node, disk, "node")
        install(args.package, disk, "dsh")
        cmd = ["env", f"LIXINOS_TCC_DISK={disk}", "LIXINOS_CODEX_SHELL=1", root / "scripts/run-kernel.sh",
               "--kernel", args.kernel, "--release", args.release, "--serial"]
        with native["guest"]["boot"](disk, cmd, prompt=b"LixinOS Bash (C shell)") as serial:
            serial.until(b"$ ")
            serial.expect(b"cd /\r", native["shell"])
            command = native["command"]
            command(serial, "dsh --version", expected=b"\n0.2.1-rc.5\n")
            command(serial, "dsh --help", expected=b"Usage: dsh [--profile]")
            command(serial, "dsh --patch", expected=b"argument missing", status=1)
            command(serial, "dsh --version > /dsh-version")
            command(serial, "cat /dsh-version", expected=b"\n0.2.1-rc.5\n")
            if args.headless:
                check_headless(serial)
                print(f"{args.kernel}: headless history, create, edit, isolated shell and writeback passed", flush=True)
            native["shutdown"](serial)
        print(f"{args.kernel}: DSH version, help, argument error and redirected output passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--headless", action="store_true", help="exercise the complete profile against a local Messages endpoint")
    parser.add_argument("--disk", type=Path, required=True)
    parser.add_argument("--package", type=Path, default=root / "Kernel/target/dsh-port/dsh-release")
    parser.add_argument("--node", type=Path, default=root / "Kernel/target/dsh-port/node-network")
    parser.add_argument("--kernel", choices=("rust", "tcc"), default="rust")
    parser.add_argument("--release", default="latest")
    check(parser.parse_args())
