"""Open, edit and save a DOCX through Win3, then verify real Writer exports."""
import argparse
import io
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
from unittest.mock import patch
from xml.etree import ElementTree
import zipfile

from disk_image import disk_entries

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "Kernel/target/user-build/libreoffice/package"
BOOT = runpy.run_path(str(ROOT / "scripts/test-panes.py"))["test"]
WRITE = runpy.run_path(str(ROOT / "User/rust/bootstrap-disk.py"))["write_volume"]
SAMPLE_TEXT = "LixinOS Writer 7319"
EDITED_TEXT = "LixinOS Writer 7400"
WORDS = ("中文读写验收", EDITED_TEXT)


def sample():
    types = "http://schemas.openxmlformats.org/package/2006/content-types"
    rels = "http://schemas.openxmlformats.org/package/2006/relationships"
    word = "http://schemas.openxmlformats.org/wordprocessingml/2006/main"
    office = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", f'<Types xmlns="{types}"><Default Extension="rels" '
                   'ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
                   '<Override PartName="/word/document.xml" '
                   'ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/></Types>')
        z.writestr("_rels/.rels", f'<Relationships xmlns="{rels}"><Relationship Id="doc" '
                   f'Type="{office}/officeDocument" Target="word/document.xml"/></Relationships>')
        z.writestr("word/document.xml", f'<w:document xmlns:w="{word}"><w:body><w:p><w:r><w:rPr>'
                   '<w:rFonts w:eastAsia="Noto Sans Mono CJK SC"/><w:b/></w:rPr>'
                   f'<w:t>{WORDS[0]}</w:t></w:r></w:p><w:p><w:r><w:t>{SAMPLE_TEXT}</w:t>'
                   '</w:r></w:p><w:sectPr><w:pgSz w:w="11906" w:h="16838"/>'
                   '</w:sectPr></w:body></w:document>')
    return out.getvalue()


def exercise(guest, control, directory):
    for command in ("writer --version", "tcc /test.c -o /bin/writer-check", "writer-check",
                    "writer --headless --convert-to odt --outdir / /sample.docx",
                    "writer --headless --convert-to pdf --outdir / /sample.docx"):
        guest.send((command + "\r").encode())
        output = guest.until(b"$ ", timeout=300)
        assert b"[exit " not in output and b"Error:" not in output, output
        if command == "writer --version":
            assert b"25.2.7.2" in output, output
    guest.send(b"q\r")
    assert guest.vm.wait(timeout=90) == 0


def run(seed, kernel, release):
    rows, _ = disk_entries(seed)
    keep = {"/bin/tcc", "/bin/bash", "/bin/sh", "/bin/shutdown", "/lib/lxe-runtime.elf", "/src/console.h"}
    prefixes = ("/boot/", "/lib/gtk3/", "/lib/tcc/", "/src/tinycc/", "/src/tcc/", "/include/")
    rows = {k: v for k, v in rows.items() if k in keep or k.startswith(prefixes) or
            (v[1] is None and k.count("/") <= 3)}
    for path in ("gui/text-clipboard.h", "desktop/window-client.h", "desktop/window-client-host.h"):
        rows["/src/" + path] = bytes(8), (ROOT / "User" / path).read_bytes()
    rows["/test.c"] = bytes(8), (ROOT / "User/writer/test-host.c").read_bytes()
    rows["/sample.docx"] = bytes(8), sample()
    if "/boot/loader.elf" not in rows:
        rows["/boot/loader.elf"] = bytes(8), (ROOT / "data/shared.loader.elf").read_bytes()
    gtk = ROOT / "Kernel/target/user-build/gtk3-port/package/lib/gtk3"
    if (gtk / "fonts.conf").is_file():
        rows["/lib/gtk3/fonts.conf"] = bytes(8), (gtk / "fonts.conf").read_bytes()
        for font in (gtk / "fonts").iterdir():
            if font.is_file():
                rows["/lib/gtk3/fonts/" + font.name] = bytes(8), font
    if not (PACKAGE / "bin/writer.win3").is_file():
        raise SystemExit("Build Writer with bash User/writer/build.sh first")
    for file in PACKAGE.rglob("*"):
        rows["/" + file.relative_to(PACKAGE).as_posix()] = bytes(8), file if file.is_file() else None
    with tempfile.TemporaryDirectory() as tmp:
        disk = Path(tmp) / "writer.img"
        WRITE(rows, b"", disk, with_loader=True)
        with patch.dict(os.environ, {"LIXINOS_TCC_DISK": str(disk)}):
            BOOT(exercise, mode="--window", disk=disk, prompt=b"$ ",
                 launcher_args=("--release", release, "--kernel", kernel))
        saved, _ = disk_entries(disk)
        for file, part in (("/sample.docx", "word/document.xml"), ("/sample.odt", "content.xml")):
            with zipfile.ZipFile(io.BytesIO(saved[file][1])) as archive:
                text = "".join(ElementTree.fromstring(archive.read(part)).itertext())
            assert all(s in text for s in WORDS), (file, text)
            assert SAMPLE_TEXT not in text, (file, text)
            if file == "/sample.docx":
                with zipfile.ZipFile(io.BytesIO(saved[file][1])) as archive:
                    xml = archive.read("word/document.xml")
                assert b'<w:jc w:val="center"' in xml, xml
                assert b'<w:sz w:val="32"' in xml, xml
                assert b'Fira Sans' in xml, xml
                assert b'Heading1' in xml, xml
                assert b'<w:u ' in xml, xml
        pdf = Path(tmp) / "export.pdf"
        pdf.write_bytes(saved["/sample.pdf"][1])
        text = subprocess.check_output(["pdftotext", "-raw", pdf, "-"]).decode()
        assert all(s in text for s in WORDS), text
        print(f"{kernel}: Win3 edit, saved DOCX, exported ODT and PDF verified")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=Path, default=ROOT / "data/shared.img")
    parser.add_argument("--release", default="latest")
    parser.add_argument("--kernel", choices=("rust", "tcc", "both"), default="both")
    args = parser.parse_args()
    for kernel in (("rust", "tcc") if args.kernel == "both" else (args.kernel,)):
        run(args.seed, kernel, args.release)
