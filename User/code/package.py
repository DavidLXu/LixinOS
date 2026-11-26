"""Stage the guest editor, its document model, and upstream notices."""
from pathlib import Path
import runpy
import shutil
import tempfile

root = Path(__file__).resolve().parents[2]
build = root / 'Kernel/target/user-build/code-port'
tool = runpy.run_path(str(root / 'User/gcc/package.py'))['tool']
with tempfile.TemporaryDirectory(dir=build) as directory:
    work = Path(directory)
    stage = work / 'package'
    for command, binary in [('/bin/clip', 'clip'), ('/bin/code', 'startcode'), ('/lib/code/editor', 'editor')]:
        tool(stage, command, build / binary, library='/lib/code',
             launcher_flag='-DLX_DBUS', build_dir=work, deflate=True)
    relink = runpy.run_path(str(root / 'User/gnome/package-mutter.py'))['compositor']
    compositor = relink(build.parent, build / 'mutter', build / 'threads.o')
    tool(stage, '/lib/code/mutter', compositor,
         library='/lib/code', launcher_flag='-DLX_DBUS', build_dir=work, deflate=True)
    home = stage / 'lib/code'
    (home / 'fonts').mkdir()
    shutil.copyfile(build / 'JetBrainsMono-Regular.ttf', home / 'fonts/JetBrainsMono-Regular.ttf')
    shutil.copyfile(build / 'font-LICENSE', home / 'fonts/LICENSE')
    for name in ('pinyin.tsv', 'pinyin-LICENSE', 'pinyin-AUTHORS'):
        shutil.copyfile(build / name, home / name)
    for source, name in [('buffer.js', 'buffer.js'), ('package/LICENSE', 'monaco-LICENSE'),
                         ('package/ThirdPartyNotices.txt', 'monaco-notices.txt'),
                         ('quickjs-2025-09-13/LICENSE', 'quickjs-LICENSE')]:
        shutil.copyfile(build / source, home / name)
    destination = build / 'guest-package'
    if destination.exists():
        shutil.rmtree(destination)
    stage.rename(destination)
