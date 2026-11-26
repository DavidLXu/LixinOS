"""Verify FreeCAD Sketcher/Part modeling, desktop events and BRep persistence."""
import argparse
import math
import os
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import sys
import tempfile
from unittest.mock import patch

from disk_image import disk_entries, edit_files

ROOT = Path(__file__).resolve().parents[1]
INSTALL = runpy.run_path(str(ROOT / 'User/gcc/install.py'))['package_edit']
BOOT = runpy.run_path(str(ROOT / 'scripts/test-user.py'))['boot']


def stage(rows):
    INSTALL(ROOT / 'Kernel/target/user-build/freecad-package', 'freecad')(rows)
    for directory in ('desktop', 'gui'):
        rows.setdefault(f'/src/{directory}'.encode(), (bytes(8), None))
    for name in ('desktop/window-client.h', 'desktop/window-client-host.h', 'gui/text-clipboard.h'):
        rows[f'/src/{name}'.encode()] = bytes(8), (ROOT / 'User' / name).read_bytes()
    rows[b'/cad-test.c'] = bytes(8), (ROOT / 'User/freecad/test-window.c').read_bytes()


def volume(guest, path, radius):
    guest.send(f'freecad-part.win3 --inspect {path}\r'.encode())
    result = guest.until(b'$ ', timeout=60)
    match = re.search(rb'^volume ([0-9.]+) mm3$', result, re.M)
    expected = (36 * 24 - math.pi * radius**2) * 8
    assert match and abs(float(match[1]) - expected) < 1e-4, result


def run_probe(seed, release, probe, prepare=None):
    build = ROOT / 'Kernel/target/user-build/freecad-port'
    pack = runpy.run_path(str(ROOT / 'User/gcc/package.py'))['tool']
    with tempfile.TemporaryDirectory() as tmp:
        workspace = Path(tmp)
        package = workspace / 'package'
        disk = workspace / 'gl.img'
        name = f'freecad-{probe}-test'
        if probe == 'desktop':
            package = build / 'desktop-package'
            subprocess.run([sys.executable, ROOT / 'User/freecad/package-desktop.py'], check=True)
            name = 'freecad'
        else:
            pack(package, '/bin/' + name, build / ('test-' + probe), library='/lib/' + name,
                 launcher_flag='-DLX_DBUS', build_dir=workspace, deflate=True)
        if probe in ('qt', 'wayland', 'view'):
            fonts = package / 'lib/freecad-qt-test/fonts'
            fonts.mkdir(parents=True, exist_ok=True)
            font = build.parent / 'gtk3-port/package/lib/gtk3/fonts/FiraSans-Regular.ttf'
            shutil.copy2(font, fonts)
        if probe == 'desktop':
            # The full executable and Python workbenches exceed the regular
            # disk profile's process arena. A fresh sparse build volume keeps
            # the test independent of the daily disk.
            volume = runpy.run_path(str(ROOT / 'User/rust/bootstrap-disk.py'))['write_volume']
            entries, tail = disk_entries(seed)
            volume(entries, tail, disk)
        else:
            subprocess.run(['cp', '-c', seed, disk], check=True)

        def install_probe(rows):
            for file in sorted(package.rglob('*')):
                name = ('/' + file.relative_to(package).as_posix()).encode()
                if file.is_file():
                    rows[name] = bytes(8), file.read_bytes()
                else:
                    rows.setdefault(name, (bytes(8), None))
        if probe in ('wayland', 'view', 'desktop'):
            mutter = runpy.run_path(str(ROOT / 'scripts/test-mutter.py'))
            mutter['install'](disk, workspace / 'mutter-pkg', workspace)
        edit_files(disk, INSTALL(package, 'freecad-desktop') if probe == 'desktop' else install_probe)
        if probe == 'desktop':
            initialize_desktop(disk, release)
        if prepare is not None:
            # APFS clones keep the eight-gigabyte build volume sparse.
            with tempfile.TemporaryDirectory(dir=prepare.parent) as staging:
                candidate = Path(staging) / "freecad.img"
                subprocess.run(["cp", "-c", disk, candidate], check=True)
                os.link(candidate, prepare)
            print(f"Prepared FreeCAD desktop disk: {prepare}", flush=True)
            return
        exercise_probe(disk, release, probe, name)


def initialize_desktop(disk, release):
    # The Rust first boot installs matching bundled commands, including the
    # Bash used by the TCC kernel on the same disk.
    command = [ROOT / 'scripts/run-kernel.sh', '--serial', '--kernel', 'rust', '--release', release]
    with BOOT(disk, command, prompt=b'$ ') as guest:
        guest.send(b'q\r')
        assert guest.vm.wait(timeout=45) == 0


def exercise_probe(disk, release, probe, name):
    if probe in ('wayland', 'view', 'desktop'):
        mutter = runpy.run_path(str(ROOT / 'scripts/test-mutter.py'))
    with tempfile.TemporaryDirectory() as directory:
        for kernel in ('rust', 'tcc'):
            boot_disk = disk
            if probe == 'desktop':
                # The macro finishes while the GUI is open. Each kernel gets the
                # same clean session, without a crash-recovery dialog on restart.
                boot_disk = Path(directory) / (kernel + '.img')
                subprocess.run(['cp', '-c', disk, boot_disk], check=True)
            args = [ROOT / 'scripts/run-kernel.sh', '--serial', '--kernel', kernel, '--release', release]
            with patch.dict(os.environ, {'LIXINOS_TCC_DISK': str(boot_disk)}), BOOT(boot_disk, args, prompt=b'$ ') as guest:
                if probe in ('wayland', 'view', 'desktop'):
                    mutter['session'](guest, f'/tmp/freecad-{kernel}')
                    mutter['GUEST']['command'](guest, 'export WAYLAND_DISPLAY=wayland-freecad '
                                               'QT_QPA_PLATFORM=wayland LIBGL_ALWAYS_SOFTWARE=1')
                    size = '1024x768' if probe == 'desktop' else '320x240'
                    command = (f'dbus-run-session -- /bin/mutter --virtual-monitor {size} '
                               f'--wayland-display wayland-freecad -- /bin/{name}')
                else:
                    command = name
                if probe == 'desktop':
                    # Close through the real Qt window after the modeling macro.
                    # Keep the installed macro usable on its own for an interactive demo.
                    close = 'from PySide6.QtCore import QTimer; QTimer.singleShot(0, FreeCADGui.getMainWindow().close)'
                    mutter['GUEST']['command'](guest, f"echo '{close}' > /tmp/freecad-quit.FCMacro")
                    guest.send((command + ' /lib/freecad-desktop/test-workbenches.FCMacro '
                                '/tmp/freecad-quit.FCMacro\r').encode())
                    output = guest.until(b'$ ', timeout=240)
                    print(kernel, output.decode(errors='replace'), flush=True)
                    assert b'FreeCAD GUI, Sketcher, Part and FCStd round trip passed' in output, output
                    guest.send(b'echo $?\r')
                    guest.until(b'\r\n0\r\n', timeout=10)
                    continue
                guest.send((command + '\r').encode())
                if probe == 'view':
                    # This fixture leaves the verified frame on the compositor.
                    # The full desktop test exercises close and process teardown.
                    marker = b'Wayland FreeCAD 3D viewport passed'
                    output = guest.until(marker, timeout=90)
                    print(kernel, (output + marker).decode(errors='replace'), flush=True)
                    continue
                output = guest.until(b'$ ', timeout=90)
                print(kernel, output.decode(errors='replace'), flush=True)
                if probe == 'wayland':
                    assert b'Wayland FreeCAD widget exposed' in output, output
                if probe in ('qt', 'wayland'):
                    assert b'FreeCAD SplitButton click and repaint passed' in output, output
                else:
                    assert b'triangle readback passed' in output, output
                if probe == 'coin':
                    assert b'cube readback passed' in output, output
                guest.send(b'echo $?\r')
                guest.until(b'\r\n0\r\n', timeout=10)
                guest.send(b'q\r')
                assert guest.vm.wait(timeout=30) == 0


def run(seed, release):
    with tempfile.TemporaryDirectory() as tmp:
        disk = Path(tmp) / 'freecad.img'
        subprocess.run(['cp', '-c', seed, disk], check=True)
        edit_files(disk, stage)
        for kernel in ('rust', 'tcc'):
            args = [ROOT / 'scripts/run-kernel.sh', '--serial', '--kernel', kernel, '--release', release]
            with patch.dict(os.environ, {'LIXINOS_TCC_DISK': str(disk)}), BOOT(disk, args, prompt=b'$ ') as guest:
                guest.expect(b'freecad-sketch-probe 6 5\r', b'$ ', b'(3.000000, 4.000000)')
                guest.expect(b'freecad-sketch-probe 8 5\r', b'$ ', b'(4.000000, 3.000000)')
                guest.expect(b'freecad-sketch-probe 10 2\r', b'$ ')
                guest.expect(b'echo $?\r', b'$ ', b'\n2\n')
                filename = f'/plate-{kernel}.brep'
                guest.expect(f'freecad-part.win3 --create {filename}\r'.encode(), b'$ ')
                guest.expect(b'echo $?\r', b'$ ', b'\n0\n')
                volume(guest, filename, 5)
                if kernel == 'rust':
                    guest.expect(b'tcc /cad-test.c -o /bin/cad-test\r', b'$ ')
                else:
                    volume(guest, '/plate-001.brep', 5.5)
                guest.send(b'cad-test\r')
                result = guest.until(b'$ ', timeout=100)
                assert b'FreeCAD Win3 render, rotate, edit, save and resize passed' in result, result
                volume(guest, '/plate-001.brep' if kernel == 'rust' else '/plate-002.brep', 5.5)
                guest.send(b'q\r')
                assert guest.vm.wait(timeout=30) == 0
                print(f'{kernel}: Sketcher, Part, Win3 and saved BRep geometry passed', flush=True)


def check_pinyin(guest, control, directory, mutter):
    import time
    qmp = mutter['qmp_connection'](control)
    qmp('qmp_capabilities')
    macro = [
        'import os',
        'from PySide6 import QtCore, QtWidgets',
        'w=FreeCADGui.getMainWindow()',
        'def ask():',
        '    d=QtWidgets.QInputDialog(w); d.setLabelText("Type in this field"); d.setWindowTitle("Pinyin")',
        '    d.setTextValue(""); f=d.findChild(QtWidgets.QLineEdit)',
        '    f.setStyleSheet("QLineEdit { background: #e9de4a; color: black; }")',
        '    QtCore.QTimer.singleShot(60000,d.accept); os.write(1,b"INPUT_READY"); d.exec()',
        '    os.write(1,("FREECAD_INPUT="+d.textValue()+" END").encode()); QtCore.QTimer.singleShot(0,w.close)',
        'QtCore.QTimer.singleShot(3500,ask)',
    ]
    for i, line in enumerate(macro):
        redirect = '>>' if i else '>'
        mutter['GUEST']['command'](guest, f"echo '{line}' {redirect} /tmp/pinyin.FCMacro", prompt=b'$ ')
    guest.send(b'freecad /tmp/pinyin.FCMacro\r')
    guest.until(b'INPUT_READY', timeout=420)
    frame = directory / "freecad-field.ppm"
    field_color = bytes.fromhex("e9de4a")
    for _ in range(40):
        qmp("screendump", filename=str(frame))
        _, dimensions, _, raw = frame.read_bytes().split(b"\n", 3)
        width, height = map(int, dimensions.split())
        # Some QEMU Cocoa modes pad each RGB scanline to four bytes.
        stride, remainder = divmod(len(raw), height)
        assert not remainder and stride in (3 * width, (3 * width + 3) & ~3)
        hits = [(x, y) for y in range(0, height, 8) for x in range(0, width, 8)
                if raw[y * stride + x * 3 : y * stride + x * 3 + 3] == field_color]
        if hits:
            break
        time.sleep(.5)
    else:
        raise AssertionError("FreeCAD input dialog did not appear")
    x = sum(point[0] for point in hits) // len(hits)
    y = sum(point[1] for point in hits) // len(hits)
    for down in (True, False):
        qmp("input-send-event", events=[
            {"type":"abs", "data":{"axis":"x", "value":x * 32767 // (width - 1)}},
            {"type":"abs", "data":{"axis":"y", "value":y * 32767 // (height - 1)}},
            {"type":"btn", "data":{"button":"left", "down":down}}])
        time.sleep(.5)
    for key in ("a", "f7", "n", "i", "h", "a", "o", "1", "ret"):
        qmp("human-monitor-command", **{"command-line": f"sendkey {key} 80"})
        time.sleep(2 if key == "f7" else .6)
    guest.until(b"FREECAD_INPUT=", timeout=180)
    actual = guest.until(b" END")
    assert actual == "a你好".encode(), actual
    guest.until(b"$ ", timeout=180)


def check_launcher(installed_disk, release, pinyin=False):
    mutter = runpy.run_path(str(ROOT / 'scripts/test-mutter.py'))

    def exercise(guest, _control, _directory):
        guest.until(b'$ ')
        if pinyin:
            check_pinyin(guest,_control,_directory,mutter)
            return
        # --desktop covers modeling and persistence. Exercise just the terminal
        # entry, argument forwarding, visible window and clean session shutdown.
        macro = ('import os; from PySide6.QtCore import QTimer; '
                 'w=FreeCADGui.getMainWindow(); '
                 'os.write(1,b"FreeCAD launcher window visible" if w.isVisible() else b"Hidden window"); '
                 'QTimer.singleShot(0,w.close)')
        mutter['GUEST']['command'](guest, f"echo '{macro}' > /tmp/freecad-launch.FCMacro", prompt=b'$ ')
        guest.send(b'freecad /tmp/freecad-launch.FCMacro\r')
        output = guest.until(b'$ ', timeout=600)
        assert b'FreeCAD launcher window visible' in output, output
        guest.send(b'echo $?\r')
        guest.until(b'\r\n0\r\n', timeout=10)

    with tempfile.TemporaryDirectory() as directory:
        for kernel in ('rust', 'tcc'):
            disk = Path(directory) / (kernel + '.img')
            subprocess.run(['cp', '-c', installed_disk, disk], check=True)
            mutter['with_window'](disk, release, kernel, exercise)
            if pinyin:
                print(f'{kernel}: FreeCAD dialog received a你好 from the QEMU keyboard', flush=True)
            else:
                print(f'{kernel}: freecad launched from a graphical shell and exited successfully', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disk', type=Path, default=ROOT / 'data/desktop-pingfang.img')
    parser.add_argument('--release', default='latest')
    probes = parser.add_mutually_exclusive_group()
    probes.add_argument('--desktop-gl', dest='probe', action='store_const', const='gl', help='verify desktop OpenGL and GLU readback')
    probes.add_argument('--desktop', dest='probe', action='store_const', const='desktop', help='verify the upstream desktop and FCStd round trip')
    probes.add_argument('--view', dest='probe', action='store_const', const='view', help='render Coin in a guest Wayland QOpenGLWidget')
    probes.add_argument('--wayland', dest='probe', action='store_const', const='wayland', help='open a Qt widget on guest Mutter')
    probes.add_argument('--qt-widget', dest='probe', action='store_const', const='qt', help='exercise an upstream FreeCAD widget')
    probes.add_argument('--coin', dest='probe', action='store_const', const='coin', help='also render a Coin3D scene')
    probes.add_argument('--prepare-desktop', metavar='NEW_IMAGE', type=Path,
                        help='prepare a persistent boot disk with FreeCAD and Mutter')
    probes.add_argument('--launcher', action='store_true',
                        help='test freecad from a graphical terminal on a prepared --disk')
    probes.add_argument('--pinyin', action='store_true',
                        help='type Chinese through QEMU in a FreeCAD text field with Code installed')
    args = parser.parse_args()
    if args.launcher or args.pinyin:
        check_launcher(args.disk, args.release, args.pinyin)
        sys.exit(0)
    if args.prepare_desktop:
        run_probe(args.disk, args.release, 'desktop', args.prepare_desktop)
        sys.exit(0)
    if args.probe:
        run_probe(args.disk, args.release, args.probe)
    else:
        run(args.disk, args.release)
