"""Boot a disposable guest and check a native Codex Responses API turn."""
import argparse
from contextlib import contextmanager, nullcontext
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import runpy
import select
import tempfile
import threading
import time
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SERIAL = runpy.run_path(str(ROOT / "scripts/test-user.py"))
VOLUME = runpy.run_path(str(ROOT / "User/rust/bootstrap-disk.py"))
COMMANDS = {"read": "cat proof.txt", "cwd": "pwd", "list": "ls",
            "escape": "cat /tmp/outside.txt", "build": "mkdir build && tcc probe.c -o build/probe && ./run-tests.sh | grep compiled"
                     " && cp payload.bin build/copy.bin && echo saved > build/result.txt && rm obsolete.txt && exit 17"}


@contextmanager
def responses_fixture(answer, race_started, race_resume, job_started, job_done):
    requests = []

    class Responses(BaseHTTPRequestHandler):
        def do_POST(self):
            if self.path != "/v1/responses":
                self.send_error(404)
                return
            payload = self.rfile.read(int(self.headers["Content-Length"]))
            requests.append(json.loads(payload))
            reply_id = f"resp_{len(requests)}"
            inputs = json.dumps(requests[-1]["input"])
            if "test-chat-hold" in inputs:
                race_started.set()
                race_resume.wait(timeout=45)
                return
            elif "test-chat-recall" in inputs or "test-chat-read" in inputs:
                outputs = [x for x in requests[-1]["input"] if x.get("type") == "function_call_output"]
                if not outputs and "test-chat-recall" not in inputs:
                    items = [{"type": "function_call", "id": "fc_read", "call_id": "read",
                              "name": "exec_command", "arguments": json.dumps({"cmd": "cat proof.txt"})}]
                else:
                    value = answer if answer in inputs and "test-chat-read" in inputs else "missing context"
                    if "TUI_FIRST_" in inputs:
                        value = ("TUI_NEXT_" if "test-chat-recall" in inputs else "TUI_FIRST_") + value
                    items = [{"type": "message", "id": f"msg_{len(requests)}", "role": "assistant",
                              "content": [{"type": "output_text", "text": value}]}]
            elif "test-workspace-race" in inputs:
                outputs = [x for x in requests[-1]["input"] if x.get("type") == "function_call_output"]
                if not outputs:
                    items = [{"type": "function_call", "call_id": "concurrent", "id": "fc_concurrent",
                              "name": "exec_command", "arguments": json.dumps({
                                  "cmd": "echo new > first.txt; sleep 2; echo job > changing.txt"})}]
                    job_started.set()
                else:
                    ok = "workspace changed" in json.dumps(outputs)
                    items = [{"type": "message", "role": "assistant", "id": "msg_conflict",
                              "content": [{"type": "output_text", "text": "workspace conflict verified" if ok else str(outputs)}]}]
                    job_done.set()
            elif "test-read-only" in inputs:
                outputs = [x for x in requests[-1]["input"] if x.get("type") == "function_call_output"]
                if not outputs:
                    items = [{"type": "function_call", "call_id": "blocked", "id": "fc_blocked",
                              "name": "exec_command", "arguments": json.dumps({"cmd": "echo BAD > forbidden.txt"})}]
                else:
                    blocked = "write denied" in json.dumps(outputs)
                    items = [{"type": "message", "role": "assistant", "id": "msg_denied",
                              "content": [{"type": "output_text", "text": "read-only verified" if blocked else str(outputs)}]}]
            elif "test-output-replace" in inputs or "test-output-race" in inputs:
                if "test-output-race" in inputs:
                    race_started.set()
                    if not race_resume.wait(timeout=45):
                        self.send_error(500)
                        return
                items = [{"type": "message", "role": "assistant", "id": "msg_output",
                          "content": [{"type": "output_text", "text": "replacement"}]}]
            elif len(requests) == 1:
                items = [{"type": "function_call", "call_id": key, "id": "fc_" + key,
                          "name": "exec_command",
                          "arguments": json.dumps({"cmd": cmd, "login": False})}
                         for key, cmd in COMMANDS.items()]
            else:
                replies = {x["call_id"]: json.dumps(x["output"]) for x in requests[-1]["input"]
                           if x.get("type") == "function_call_output"}
                expected = {"read": answer, "cwd": "/home/lixin", "list": "proof.txt",
                            "escape": "outside the readable", "build": "compiled:" + answer}
                matches = all(text in replies.get(key, "") for key, text in expected.items())
                items = [{"type": "message", "role": "assistant", "id": "msg_guest",
                          "content": [{"type": "output_text",
                                       "text": answer if matches else str(replies)}]}]
            events = [{"type": "response.created", "response": {"id": reply_id}}]
            events.extend({"type": "response.output_item.done", "item": item} for item in items)
            events.append({"type": "response.completed", "response": {"id": reply_id,
                           "usage": {"input_tokens": 1, "output_tokens": 1, "total_tokens": 2}}})
            body = "".join("data: " + json.dumps(e) + "\n\n" for e in events).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):
            pass

    with ThreadingHTTPServer(("127.0.0.1", 0), Responses) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            yield server.server_port, requests
        finally:
            server.shutdown()
            thread.join()


def tui_screen_reader(guest):
    import pyte

    screen = pyte.Screen(80, 24)
    stream = pyte.ByteStream(screen)
    stream.feed(guest.buffer)
    guest.buffer = b""

    def expect(message, timeout=150):
        deadline = time.monotonic() + timeout
        while True:
            display = "\n".join(screen.display)
            if message in display:
                return
            if "shell: service unavailable" in display or "lixin:/home/lixin$ " in display:
                raise RuntimeError("TUI returned to the shell early\n" + display)
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([guest.vm.stdout], [], [], remaining)[0]:
                raise TimeoutError("Waiting for " + message + "\n" + display)
            chunk = os.read(guest.vm.stdout.fileno(), 16384)
            if not chunk:
                raise EOFError(display)
            stream.feed(chunk)

    return expect


def verify(package, kernel, release, directory, live=False, interactive=False, tui=False):
    answer = "LIXINOS_OK_" + os.urandom(8).hex()
    race_started, race_resume = threading.Event(), threading.Event()
    job_started, job_done = threading.Event(), threading.Event()
    fixture = (nullcontext((None, [])) if live else
               responses_fixture(answer, race_started, race_resume, job_started, job_done))
    with fixture as (port, requests):
        config = f'''model = "gpt-5.4"
model_provider = "fixture"
[model_providers.fixture]
name = "Local Responses fixture"
base_url = "http://10.0.2.2:{port}/v1"
wire_api = "responses"
request_max_retries = 0
stream_max_retries = 0
'''
        prompt = '"Use exec_command with cat proof.txt and reply with only the exact file contents."'
        if live:
            config = Path(__file__).with_name("deepseek.toml").read_text()
            prompt = '"Run tcc probe.c -o probe && ./probe using exec_command. Return only the text after compiled:."'
        if tui:
            # This disposable project contains only the generated proof file and test inputs.
            config += '\n[projects."/home/lixin"]\ntrust_level = "trusted"\n'
        stamp = VOLUME["stamp"]
        rows = {p: (stamp, None) for p in
                ("/", "/home", "/home/lixin", "/home/lixin/.codex", "/tmp")}
        rows["/home/lixin/.codex/config.toml"] = stamp, config.encode()
        rows["/home/lixin/proof.txt"] = stamp, answer.encode() + b"\n"
        rows["/home/lixin/run-tests.sh"] = stamp, b"#!/bin/sh\n./build/probe isolated\n"
        if not live:
            rows["/home/lixin/payload.bin"] = stamp, os.urandom(100_000)
        rows["/home/lixin/obsolete.txt"] = stamp, b"remove during test"
        rows["/home/lixin/probe.c"] = stamp, (
            '''#include <stdio.h>
int main(int argc, char **argv) {
    if (argc > 1 && (fopen("/tmp/outside.txt","r") || fopen(".codex/config.toml","r"))) return 89;
    puts("compiled:''' + answer + '");return 0;}'
        ).encode()
        rows["/tmp/outside.txt"] = stamp, b"out-of-workspace marker"
        # Exporting one setting must preserve the guest home used for provider discovery.
        rows["/home/lixin/.profile"] = stamp, b"export CODEX_SMOKE_TEST=1\n"
        for entry in package.iterdir():
            VOLUME["tree"](rows, entry, "/" + entry.name)
        disk = directory / "codex.img"
        VOLUME["write_volume"](rows, b"", disk)

        def boot(flavor):
            command = [ROOT / "scripts/run-kernel.sh", "--kernel", flavor, "--serial",
                       "--release", release]
            return SERIAL["boot"](disk, command, prompt=b"lixin:/home/lixin$ ")

        # A fresh disk gets its built-in shell and LXE runtime on its first Rust boot.
        # The TCC kernel loads both from disk.
        if kernel == "tcc":
            with boot("rust") as guest:
                guest.send(b"q\r")
                if guest.vm.wait(timeout=45) != 0:
                    raise RuntimeError("guest did not save its initial filesystem")
        with patch.dict(os.environ, LIXINOS_TCC_DISK=str(disk),
                        LIXINOS_CODEX_SHELL="1", LIXINOS_CODEX_DEEPSEEK="1" if live else "0"), boot(kernel) as guest:
            if tui:
                shell = b"lixin:/home/lixin$ "
                guest.send(b'codex "Read proof.txt with exec_command. Reply TUI_FIRST_ immediately followed by its contents. test-chat-read"\r')
                guest.until(b"\x1b[?1049h", timeout=90)
                print(kernel, "entered alternate screen", flush=True)
                expect_screen = tui_screen_reader(guest)
                expect_screen("TUI_FIRST_" + answer)
                print(kernel, "TUI rendered the tool result", flush=True)
                # Use bracketed paste so a burst of serial bytes has the same meaning as one paste.
                guest.send(b"\x1b[200~Repeat your previous answer replacing TUI_FIRST_ with TUI_NEXT_. test-chat-recall\x1b[201~")
                expect_screen("Repeat your previous answer")
                guest.send(b"\r")
                expect_screen("TUI_NEXT_" + answer)
                if not live and not any("test-chat-recall" in json.dumps(req["input"])
                                        and answer in json.dumps(req["input"]) for req in requests):
                    raise AssertionError("follow-up did not carry the previous tool result")
                guest.send(b"\x1b[200~/exit\x1b[201~")
                expect_screen("/exit")
                guest.send(b"\r")
                guest.until(b"\x1b[?1049l", timeout=30)
                guest.until(shell)
                guest.expect(b"echo $?\r", shell, b"\n0\n")
                print(kernel, "TUI follow-up, alternate-screen restore and clean exit verified", flush=True)
                return
            if interactive:
                shell = b"lixin:/home/lixin$ "
                guest.send(b"codex line\r")
                guest.until(b"you> ", timeout=90)
                guest.expect(b"/help\r", b"you> ", b"Ctrl-D")
                print(kernel, "interactive prompt ready", flush=True)
                turns = (b"Read proof.txt with exec_command. Reply only with its contents. test-chat-read",
                         b"Repeat your last answer from memory, without calling tools. test-chat-recall")
                for prompt_text in turns:
                    guest.send(prompt_text + b"\r")
                    received = guest.until(b"you> ", timeout=150)
                    if answer.encode() not in received:
                        raise AssertionError(received[-3000:])
                    print(kernel, "verified", prompt_text.split()[-1].decode(), flush=True)
                if not live:
                    guest.send(b"test-chat-hold\r")
                    try:
                        if not race_started.wait(60):
                            raise AssertionError("turn did not reach the fixture")
                        guest.send(b"\x03")
                        received = guest.until(b"you> ", timeout=30)
                        if b"interrupted" not in received.lower():
                            raise AssertionError(received)
                    finally:
                        race_resume.set()
                guest.expect(b"/exit\r", shell)
                guest.expect(b"echo $?\r", shell, b"\n0\n")
                guest.send(b"codex line\r")
                guest.until(b"you> ", timeout=90)
                guest.expect(b"\x04", shell)
                print(kernel, "interactive tool call, follow-up and shell return verified", flush=True)
                return
            for argv, expected in (
                ("codex --version", b"0.171.0"),
                ("codex exec --help", b"--skip-git-repo-check"),
                (f"codex exec --skip-git-repo-check --ephemeral --json --sandbox workspace-write -o result.txt {prompt}", answer.encode()),
            ):
                guest.send(argv.encode() + b"\r")
                received = guest.until(b"lixin:/home/lixin$ ", timeout=180)
                if expected not in received:
                    raise AssertionError(received[-3500:])
                if "--json" in argv:
                    events = [json.loads(line) for line in received.splitlines()
                              if line.startswith(b"{")]
                    if not any(e.get("type") == "turn.completed" for e in events):
                        raise AssertionError(events)
                    if not any(e.get("item", {}).get("text", "").strip() == answer
                               for e in events if e.get("type") == "item.completed"):
                        raise AssertionError("No matching completed assistant message")
                    if live:
                        print(kernel, "DeepSeek answered", answer,
                              next(e["usage"] for e in events if e["type"] == "turn.completed"), flush=True)
                    guest.send(b"echo $?\r")
                    if b"\n0\n" not in guest.until(b"lixin:/home/lixin$ "):
                        raise AssertionError("codex exec exited unsuccessfully")
                print(f"{kernel}: {argv} passed", flush=True)
            guest.expect(b"cat result.txt\r", b"lixin:/home/lixin$ ", answer.encode())
            if live:
                guest.expect(b"./probe\r", b"lixin:/home/lixin$ ", b"compiled:" + answer.encode())
            if not live:
                shell = b"lixin:/home/lixin$ "
                if not any(e.get("item", {}).get("exit_code") == 17
                           for e in events if e.get("type") == "item.completed"):
                    raise AssertionError("shell tool lost the exit status")
                guest.expect(b"cmp payload.bin build/copy.bin\r", shell)
                guest.expect(b"echo $?\r", shell, b"\n0\n")
                guest.expect(b"cat build/result.txt\r", shell, b"saved")
                guest.send(b"ls -1\r")
                if b"obsolete.txt" in guest.until(shell):
                    raise AssertionError("removed file survived writeback")
                guest.expect(b"./build/probe\r", shell, b"compiled:" + answer.encode())
                guest.send(b"codex exec --skip-git-repo-check --ephemeral --sandbox read-only test-read-only\r")
                result = guest.until(shell, timeout=120)
                if b"read-only verified" not in result:
                    raise AssertionError(result[-2200:])
                guest.send(b"codex exec --skip-git-repo-check --ephemeral --sandbox workspace-write test-workspace-race &\r")
                guest.until(shell, timeout=45)
                if not job_started.wait(45):
                    raise AssertionError("workspace race did not start")
                deadline = time.monotonic() + 65
                iteration = 0
                while not job_done.wait(0.2):
                    if time.monotonic() > deadline:
                        raise AssertionError("workspace race did not finish")
                    iteration += 1
                    guest.expect(f"echo foreground-{iteration} > changing.txt\r".encode(), shell)
                outcomes = [x["output"] for request in requests for x in request.get("input", [])
                            if x.get("type") == "function_call_output" and x.get("call_id") == "concurrent"]
                if not any("workspace changed" in json.dumps(output) for output in outcomes):
                    raise AssertionError(("expected a write conflict", outcomes))
                # A background shell can print its final response during the last foreground echo.
                # Inspect persisted files and the actual tool result instead of waiting for that text twice.
                guest.expect(b"cat changing.txt\r", shell, f"foreground-{iteration}".encode())
                guest.send(b"ls -1\r")
                if b"first.txt" in guest.until(shell):
                    raise AssertionError("a failed workspace transaction partially committed")
                exe = b"codex exec --skip-git-repo-check --ephemeral -o result.txt "
                guest.send(exe + b"test-output-replace\r")
                guest.until(shell, timeout=120)
                guest.expect(b"echo $?\r", shell, b"\n0\n")
                guest.expect(b"cat result.txt\r", shell, b"replacement")
                guest.send(exe + b"test-output-race &\r")
                guest.until(shell, timeout=45)
                try:
                    if not race_started.wait(timeout=45):
                        raise AssertionError("the background turn did not reach the fixture")
                    guest.expect(b"echo concurrent-change > result.txt\r", shell)
                finally:
                    race_resume.set()
                guest.until(b"Failed to write last message", timeout=60)
                guest.expect(b"cat result.txt\r", shell, b"concurrent-change")
            guest.send(b"ls -1\r")
            if b"forbidden.txt" in guest.until(b"lixin:/home/lixin$ "):
                raise AssertionError("the unsupported redirect created a file")
            print(f"{kernel}: verified saved output", flush=True)
        if not live and len(requests) != 8:
            raise AssertionError("unexpected number of fixture requests")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("--kernel", choices=["rust", "tcc"], required=True)
    parser.add_argument("--release", default="latest")
    parser.add_argument("--live", action="store_true", help="Use the macOS DeepSeek Keychain item on an isolated test disk")
    parser.add_argument("--interactive", action="store_true", help="Check multi-turn terminal chat")
    parser.add_argument("--tui", action="store_true", help="Exercise the upstream fullscreen TUI")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as temp:
        verify(args.package.resolve(), args.kernel, args.release, Path(temp), args.live, args.interactive, args.tui)
