"""Validate QEMU's 48 kHz stereo PCM capture after the guest powers off."""
from array import array
from pathlib import Path
import struct
import sys


def check_samples(path: Path):
    raw = path.read_bytes()
    assert raw[:4] == b"RIFF" and raw[8:12] == b"WAVE", "missing WAV container"
    offset = 12
    fmt = None
    while offset + 8 <= len(raw):
        chunk, size = struct.unpack_from("<4sI", raw, offset)
        offset += 8
        if chunk == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", raw, offset)
        if chunk == b"data":
            assert fmt == (1, 2, 48000, 192000, 4, 16), fmt
            # QEMU may leave this field zero when the guest exits through PSCI.
            samples = array("h", raw[offset:offset+size] if size else raw[offset:])
            if sys.byteorder == "big":
                samples.byteswap()
            assert sum(abs(s) > 200 for s in samples) >= 480, "silent or truncated playback"
            return len(samples) // 2
        offset += size + (size & 1)
    raise AssertionError("missing WAV data")
