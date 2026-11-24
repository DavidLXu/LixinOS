#!/usr/bin/env python3
"""Relay one guest Responses request. Only the host reads the Keychain item."""
import os
import pwd
import http.client
import re
import subprocess
import sys

LIMIT = 16 << 20


def reply(out, code, body):
    out.write(f"HTTP/1.1 {code}\r\nConnection: close\r\nContent-Type: text/plain\r\nContent-Length: {len(body)}\r\n\r\n".encode() + body)
    out.flush()


def serve(stream, out):
    if stream.readline(256) != b"POST /v1/responses HTTP/1.1\r\n":
        return reply(out, "404 Not Found", b"Use POST /v1/responses")
    headers = {}
    for _ in range(64):
        line = stream.readline(8193)
        if line == b"\r\n":
            break
        name, sep, value = line.partition(b":")
        name = name.lower()
        if (len(line) > 8192 or not line.endswith(b"\r\n") or not sep
                or not re.fullmatch(rb"[a-z0-9-]+", name) or name in headers):
            return reply(out, "400 Bad Request", b"Invalid request headers")
        headers[name] = value.strip()
    else:
        return reply(out, "431 Request Header Fields Too Large", b"Too many headers")
    length = headers.get(b"content-length", b"")
    if b"transfer-encoding" in headers or not re.fullmatch(rb"[0-9]{1,8}", length):
        return reply(out, "411 Length Required", b"A single Content-Length is required")
    if int(length) > LIMIT:
        return reply(out, "413 Content Too Large", b"Request exceeds relay limit")
    body = stream.read(int(length))
    if len(body) != int(length):
        return reply(out, "400 Bad Request", b"Incomplete request")
    if headers.get(b"content-encoding", b"identity") != b"identity":
        return reply(out, "415 Unsupported Media Type", b"Send uncompressed JSON")
    account = pwd.getpwuid(os.getuid()).pw_name
    try:
        lookup = subprocess.run(["/usr/bin/security", "find-generic-password",
                                 "-s", "LixinOS.DeepSeek", "-a", account, "-w"],
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                timeout=45, check=True)
    except (OSError, subprocess.SubprocessError):
        return reply(out, "503 Service Unavailable", b"DeepSeek Keychain item unavailable")
    key = lookup.stdout.rstrip(b"\r\n")
    if not re.fullmatch(rb"[!-~]+", key):
        return reply(out, "503 Service Unavailable", b"Invalid DeepSeek Keychain item")
    upstream = http.client.HTTPSConnection("api.deepseek.com", timeout=120)
    started = False
    try:
        upstream.request("POST", "/responses", body, {"Content-Type": "application/json",
                         "Accept": "text/event-stream", "Authorization": b"Bearer " + key})
        response = upstream.getresponse()
        if response.status != 200:
            return reply(out, f"{response.status} Upstream Error", b"DeepSeek rejected the Responses request")
        mime = response.getheader("Content-Type", "").split(";")[0].strip()
        if mime not in ("text/event-stream", "application/json"):
            return reply(out, "502 Bad Gateway", b"Unexpected upstream response type")
        started = True
        out.write(f"HTTP/1.1 200 OK\r\nContent-Type: {mime}\r\nConnection: close\r\nTransfer-Encoding: chunked\r\n\r\n".encode())
        while chunk := response.read1(16384):
            out.write(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n")
            out.flush()
        out.write(b"0\r\n\r\n")
        out.flush()
    except (OSError, http.client.HTTPException):
        if not started:
            reply(out, "502 Bad Gateway", b"Cannot reach DeepSeek")
    finally:
        upstream.close()


if __name__ == "__main__":
    serve(sys.stdin.buffer, sys.stdout.buffer)
