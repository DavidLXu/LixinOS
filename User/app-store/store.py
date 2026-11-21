"""Project and package operations for the LixinOS App Store."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys
import tempfile
from urllib.request import urlopen

WORK = Path("/work/store")
BENCH = Path("/src/agent/desktop.py")
LIMIT = 32768


def row(kind, *values):
    print("\t".join([kind, *[" ".join(str(v).split()) for v in values]]), flush=True)


def workbench():
    spec = importlib.util.spec_from_file_location("desktop_workbench", BENCH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def project(key):
    if not re.fullmatch(r"(vibe|port)-[a-f0-9]{10}", key):
        raise ValueError("无效的项目编号")
    path = WORK / key
    meta = json.loads((path / ".desktop/store.json").read_bytes())
    return path, meta


def upstream(source):
    if source.startswith("https://"):
        stream = urlopen(source, timeout=20)
    elif source.startswith("/") and Path(source).is_file():
        stream = Path(source).open("rb")
    else:
        raise ValueError("请填写系统内的 C 文件绝对路径，或 HTTPS 源码直链")
    with stream:
        data = stream.read(LIMIT + 1)
    if not data.strip() or len(data) > LIMIT or b"\0" in data:
        raise ValueError("目前支持 1 至 32768 字节的单文件 C 源码")
    data.decode("utf-8")
    if data.lstrip().lower().startswith((b"<!doctype", b"<html")):
        raise ValueError("此地址是网页，请填写原始 C 源码链接")
    return data


def create(mode, brief, source=""):
    if mode not in ("vibe", "port") or not brief.strip() or len(brief.encode()) > 1024:
        raise ValueError("请用 1 至 1024 字节描述想做的软件")
    original = upstream(source) if mode == "port" else None
    WORK.mkdir(parents=True, exist_ok=True)
    key = mode + "-" + secrets.token_hex(5)
    root = WORK / key
    # Only expose a complete draft, including its upstream and request.
    with tempfile.TemporaryDirectory(dir=WORK) as tmp:
        stage = Path(tmp) / key
        workbench().create(stage)
        if original is not None:
            (stage / ".desktop/upstream.c").write_bytes(original)
            (stage / "app.c").write_bytes(original)
        (stage / ".desktop/store.json").write_text(json.dumps(
            dict(brief=brief, source=source, upstream_sha256=hashlib.sha256(original).hexdigest() if original else ""),
            ensure_ascii=False))
        os.chdir(WORK)
        stage.rename(root)
    row("PROJECT", key)


def template_draft(root, meta):
    return not meta["source"] and (root / "app.c").read_bytes() == Path("/src/desktop/app-template.c").read_bytes()


def details(key):
    root, meta = project(key)
    os.chdir(root)
    p = workbench().Project()
    state = p.active()
    try:
        version, _ = p.candidate(state)
        if template_draft(root, meta):
            version = ""
    except (ValueError, FileNotFoundError):
        version = ""
    row("DETAIL", key, meta["brief"], state["current"] or "-", version or "-", state["previous"] or "-")


def library():
    rows = []
    if WORK.is_dir():
        for path in sorted(WORK.iterdir()):
            if not (path / ".desktop/store.json").is_file():
                continue
            _, meta = project(path.name)
            rows.append(("project", path.name, meta["brief"]))
    for manifest in sorted(Path("/lib").glob("*/installed.json")):
        rows.append(("package", manifest.parent.name, manifest.parent.name))
    return rows


def packages(name):
    if not re.fullmatch(r"[a-z0-9][a-z0-9-]{0,63}", name):
        raise ValueError("无效的软件包名称")
    files = json.loads((Path("/lib") / name / "installed.json").read_bytes())
    for file in sorted(files):
        if file.startswith("/bin/"):
            row("BIN", file)
    row("COUNT", len(files))


def manage(op, key, argument):
    root, meta = project(key)
    os.chdir(root)
    p = workbench().Project()
    if op == "send":
        task = argument.strip() or meta["brief"]
        if not task or len(task.encode()) > 1400:
            raise ValueError("请缩短需求后重试")
        if meta["source"]:
            task = "Port the upstream C code in app.c to a native window. Preserve upstream logic and license. " + task
        task += " Implement the requested behavior and its regression tests, then run check_app."
        # Keep diagnostics out of the binary window protocol and available on retry.
        argv = ["/bin/agent", "desktop-work", str(root), "send", task, ""]
        with subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as child:
            tail = bytearray()
            while chunk := child.stdout.read(4096):
                tail.extend(chunk)
                del tail[:-8192]
            code = child.wait()
        (root / ".desktop/store.log").write_bytes(tail)
        if code:
            lines = tail.decode(errors="replace").splitlines()
            reason = re.sub(r"\x1b\[[0-9;]*m", "", lines[-1]) if lines else ""
            raise ValueError("Agent 未完成，草稿已保留。" + reason)
    elif op == "check":
        if template_draft(root, meta):
            raise ValueError("项目仍是初始模板，请先让 Agent 修改代码")
        p.check()
    elif op in ("publish", "rollback", "remove"):
        if not re.fullmatch(r"[0-9a-f]{64}", argument):
            raise ValueError("请重新查看要操作的版本")
        if op == "remove":
            lock = Path("/bin") / ("." + p.name + ".desktop-lock")
            lock.mkdir()
            try:
                if p.active()["current"] != argument:
                    raise ValueError("已安装版本发生变化")
                subprocess.run(["/bin/rm", "/bin/" + p.app, "/bin/" + p.state], check=True)
            finally:
                lock.rmdir()
        else:
            if op == "publish" and template_draft(root, meta):
                raise ValueError("请先完成应用功能，再安装")
            p.release(op, argument)
    else:
        raise ValueError("未知操作")
    details(key)


def main(args):
    match args:
        case ["new", mode, brief, source]:
            create(mode, brief, source)
        case ["library", page]:
            rows = library()
            start = max(0, min(int(page), max(0, (len(rows)-1)//5)))*5
            row("PAGE", start//5, len(rows))
            for item in rows[start:start+5]:
                row("ITEM", *item)
        case ["package", name]:
            packages(name)
        case ["detail", key]:
            details(key)
        case [op, key, argument] if op in ("send", "check", "publish", "rollback", "remove"):
            # A turn and a deployment must not race another store window.
            root, _ = project(key)
            lock = root / ".desktop/store-lock"
            try:
                lock.mkdir()
            except FileExistsError:
                raise ValueError("此项目正在另一项操作中") from None
            try:
                manage(op, key, argument)
            finally:
                lock.rmdir()
        case _:
            raise ValueError("未知商店命令")


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        row("ERROR", str(error))
        sys.exit(1)
