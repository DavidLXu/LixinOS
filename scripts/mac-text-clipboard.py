#!/usr/bin/env python3
"""One request from the opt-in graphical VM clipboard forward."""
import struct
import subprocess
import sys

MAXIMUM = 1024 * 1024


def serve(source, sink):
    header = source.read(5)
    if len(header) != 5:
        return
    operation, size = struct.unpack("!cI", header)
    try:
        if operation == b"C" and size <= MAXIMUM:
            data = source.read(size)
            if len(data) != size:
                raise ValueError("short request")
            data.decode("utf-8")
            subprocess.run(["pbcopy"], input=data, check=True, timeout=3)
            data = b""
        elif operation == b"P" and size == 0:
            with subprocess.Popen(["pbpaste"], stdout=subprocess.PIPE) as proc:
                data = proc.stdout.read(MAXIMUM + 1)
                if len(data) > MAXIMUM:
                    proc.kill()
                    raise ValueError("clipboard too large")
                if proc.wait(timeout=3):
                    raise ValueError("pbpaste failed")
            data.decode("utf-8")
        else:
            raise ValueError("invalid request")
        sink.write(struct.pack("!cI", b"K", len(data)) + data)
    except (OSError, ValueError, subprocess.SubprocessError):
        sink.write(struct.pack("!cI", b"E", 0))


if __name__ == "__main__":
    serve(sys.stdin.buffer, sys.stdout.buffer)
