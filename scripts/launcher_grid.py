"""Drive the native Desktop application grid in QMP integration tests."""
from pathlib import Path
import re
import time


def installed_apps(output):
    names = re.sub(rb"\x1b\[[0-9;]*m", b"", output).splitlines()
    catalog = Path(__file__).resolve().parents[1] / "User/desktop/launcher-programs.def"
    programs = re.findall(rb'^PROGRAM\("([^"]+)"', catalog.read_bytes(), re.M)
    return [b"agent.win3", *sorted(n for n in names if n != b"agent.win3" and
            (n in programs or n == b"nxeyes" or n.endswith((b".win", b".win2", b".win3"))))]


def select_icon(click, qmp, width, height, index, count):
    columns = max(1, min(6, (width-48)//145))
    cell = (width-48)//columns
    x = 24 + (index % columns)*cell + cell//2
    y = 108 + (index//columns)*136 + 47
    limit = max(0, 120 + ((count+columns-1)//columns)*136 - height)
    offset = 0
    while y-offset > height-16 and offset < limit:
        for down in (True, False):
            qmp("input-send-event", events=[{"type": "btn", "data": {"button": "wheel-down", "down": down}}])
        offset = min(limit, offset+48)
        time.sleep(.08)
    click(x, y-offset)
