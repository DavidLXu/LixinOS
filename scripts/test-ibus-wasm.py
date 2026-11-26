"""Run the upstream IBus Compose engine and a WASM module in both guests."""
import argparse
from pathlib import Path
import runpy
import subprocess
import tempfile
from disk_image import edit_files, prepare

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "Kernel/target/user-build"
h = runpy.run_path(str(ROOT / "scripts/test-kernel-threads.py"))
accounts = runpy.run_path(str(ROOT / "User/accounts/install.py"))
SUM = bytes.fromhex("0061736d01000000 01070160027f7f017f 03020100 0707010373756d0000 0a09010700200020016a0b")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rust", required=True, type=Path)
    parser.add_argument("--tcc", required=True, type=Path)
    parser.add_argument("--widget",action="store_true",help="build and run the GTK input fixture")
    parser.add_argument("--widget-package",type=Path,help="include the GtkEntry integration test")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        if args.widget:
            args.widget_package=runpy.run_path(str(ROOT/"User/gtk3/build-ime-probe.py"))["build"](Path(tmp))
        seed = Path(tmp) / "base.img"
        prepare(seed)
        with h["boot"](seed,h["args"](args.rust,seed,2,"1G")) as g:
            h["native"]["shutdown"](g)
        def stage(rows):
            accounts["bootstrap"](rows)
            for package, name in (("dynlink-port/package","dynlink"), ("wasm3-port/package","wasm3"),
                                  ("gnome-port/ibus-package","ibus")):
                accounts["package_edit"](BUILD/package,name)(rows)
            rows[b"/sum.wasm"] = bytes(8), SUM
            if args.widget_package:
                for f in args.widget_package.rglob("*"):
                    path = ("/"+str(f.relative_to(args.widget_package))).encode()
                    rows[path]=bytes(8),f.read_bytes() if f.is_file() else None
                for name in ("lib/gtk3","lib/gtk3/fonts","lib/gtk3/fonts.conf","lib/gtk3/fonts/FiraSans-Regular.ttf"):
                    f = BUILD/"gtk3-port/package"/name
                    rows[("/"+name).encode()]=bytes(8),f.read_bytes() if f.is_file() else None
        edit_files(seed,stage)
        for variant,kernel in (("rust",args.rust),("tcc",args.tcc)):
            disk = Path(tmp)/(variant+".img")
            subprocess.run(["cp","-c",seed,disk],check=True)
            with h["boot"](disk,h["args"](kernel,disk,2,"2G"),prompt=b"$ ") as g:
                def execute(cmd, expected=None):
                    g.send(cmd.encode()+b"\r")
                    out = g.until(b"$ ",timeout=45)
                    print(variant,cmd,out[-260:],flush=True)
                    if expected is not None and expected not in out: raise AssertionError(out)
                execute("wasm3 --func sum /sum.wasm 17 25",b"Result: 42\n")
                if args.widget_package:
                    execute("ibus-run gtk-ime-check","GtkEntry via ibus: é".encode())
                    execute("echo $?",b"\n0\n")
                execute("export IBUS_COMPONENT_PATH=/lib/ibus/component IBUS_ADDRESS_FILE=/tmp/ibus.address")
                execute("ibus-daemon --single --address=unix:path=/tmp/ibus.sock --cache=none &")
                execute("sleep 1")
                execute("ps")
                execute("cat /tmp/ibus.address",b"IBUS_ADDRESS=")
                execute("export IBUS_ADDRESS=unix:path=/tmp/ibus.sock LIXINOS_COMPOSE_KEY=F12")
                execute("ibus-engine-simple &")
                execute("sleep 1")
                execute("ibus-input Multi_key apostrophe e","é".encode())
                if args.widget_package:
                    execute("gtk-ime-check","GtkEntry via ibus: é".encode())
                    execute("echo $?",b"\n0\n")
                print(variant,"wasm3 sum=42, IBus Compose=é",flush=True)
                h["native"]["shutdown"](g)


if __name__ == "__main__":
    main()
