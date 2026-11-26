#!/usr/bin/env python3
"""Relay one guest Responses request. Only the host reads the Keychain item."""
import os
import pwd
import http.client
import re
import subprocess
import sys

LIMIT = 16 << 20


def reply(out, code, body, keep_alive=False):
    conn = "keep-alive" if keep_alive else "close"
    out.write(f"HTTP/1.1 {code}\r\nConnection: {conn}\r\nContent-Type: text/plain\r\nContent-Length: {len(body)}\r\n\r\n".encode() + body)
    out.flush()


def lookup_key():
    account = pwd.getpwuid(os.getuid()).pw_name
    try:
        lookup = subprocess.run(["/usr/bin/security", "find-generic-password",
                                 "-s", "LixinOS.DeepSeek", "-a", account, "-w"],
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                timeout=45, check=True)
    except (OSError, subprocess.SubprocessError):
        return None, b"DeepSeek Keychain item unavailable"
    key = lookup.stdout.rstrip(b"\r\n")
    if not re.fullmatch(rb"[!-~]+", key):
        return None, b"Invalid DeepSeek Keychain item"
    return key, None


def serve(stream, out):
    key = None
    upstream = None
    try:
        while True:
            request_line = stream.readline(256)
            if not request_line:
                return
            if request_line != b"POST /v1/responses HTTP/1.1\r\n":
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
            keep_alive = b"close" not in headers.get(b"connection", b"").lower()
            if key is None:
                key, key_err = lookup_key()
                if key is None:
                    return reply(out, "503 Service Unavailable", key_err)
            started = False
            req_headers = {
                "Content-Type": "application/json",
                "Accept": "text/event-stream",
                "Connection": "keep-alive",
                "Authorization": b"Bearer " + key,
            }
            for attempt in range(2):
                if upstream is None:
                    upstream = http.client.HTTPSConnection("api.deepseek.com", timeout=120)
                try:
                    upstream.request("POST", "/responses", body, req_headers)
                    response = upstream.getresponse()
                except (OSError, http.client.HTTPException):
                    try:
                        upstream.close()
                    except OSError:
                        pass
                    upstream = None
                    if attempt == 0:
                        continue
                    return reply(out, "502 Bad Gateway", b"Cannot reach DeepSeek")
                break
            try:
                if response.status != 200:
                    detail = response.read(4096)
                    upstream_closed = response.will_close or len(detail) == 4096
                    if upstream_closed:
                        upstream.close()
                        upstream = None
                    if not detail or key in detail:
                        detail = b"DeepSeek rejected the Responses request"
                    reply(out, f"{response.status} Upstream Error", detail,
                          keep_alive=keep_alive and not upstream_closed)
                    if not keep_alive or upstream_closed:
                        return
                    continue
                mime = response.getheader("Content-Type", "").split(";")[0].strip()
                if mime not in ("text/event-stream", "application/json"):
                    upstream.close()
                    upstream = None
                    return reply(out, "502 Bad Gateway", b"Unexpected upstream response type")
                started = True
                conn = "keep-alive" if keep_alive else "close"
                out.write(f"HTTP/1.1 200 OK\r\nContent-Type: {mime}\r\nConnection: {conn}\r\nTransfer-Encoding: chunked\r\n\r\n".encode())
                while chunk := response.read1(16384):
                    out.write(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n")
                    out.flush()
                out.write(b"0\r\n\r\n")
                out.flush()
                if response.will_close:
                    upstream.close()
                    upstream = None
                if not keep_alive:
                    return
            except (OSError, http.client.HTTPException):
                if upstream is not None:
                    try:
                        upstream.close()
                    except OSError:
                        pass
                    upstream = None
                if not started:
                    reply(out, "502 Bad Gateway", b"Cannot reach DeepSeek")
                return
    finally:
        if upstream is not None:
            try:
                upstream.close()
            except OSError:
                pass


if __name__ == "__main__":
    serve(sys.stdin.buffer, sys.stdout.buffer)
