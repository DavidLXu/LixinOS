"""Exercise password provisioning, Linux-PAM and an ordinary-user shell on disposable disks."""
import argparse
import runpy
import secrets
import subprocess
import tempfile
from pathlib import Path
from disk_image import edit_files, prepare

ROOT = Path(__file__).resolve().parents[1]
h = runpy.run_path(str(ROOT / "scripts/test-kernel-threads.py"))
accounts = runpy.run_path(str(ROOT / "User/accounts/install.py"))
cache = ROOT / "Kernel/target/user-build"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rust", required=True, type=Path)
    parser.add_argument("--tcc", type=Path)
    parser.add_argument("--gdm",action="store_true",help="exercise the upstream GDM D-Bus proxies and PAM seat backend")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as temporary:
        work = Path(temporary)
        source = work / "fixture.img"
        prepare(source)
        with h["boot"](source,h["args"](args.rust,source,2,"1G")) as g:
            h["native"]["command"](g,"echo ok > /initial")
            h["native"]["shutdown"](g)
        def install(rows):
            accounts["bootstrap"](rows)
            accounts["package_edit"](cache / "dynlink-port/package","dynlink")(rows)
            accounts["package_edit"](cache / "pam-port/package","pam")(rows)
            if args.gdm:
                accounts["package_edit"](cache/"gnome-port/gdm-backend/package","gdm")(rows)
                accounts["package_edit"](cache/"dbus-port/package","dbus")(rows)
        edit_files(source,install)
        for name,kernel in (("rust",args.rust),("tcc",args.tcc)):
            if not kernel: continue
            disk = work / (name+".img")
            subprocess.run(["cp","-c",source,disk],check=True)
            with h["boot"](disk,h["args"](kernel,disk,2,"1536M"),prompt=b"$ ") as g:
                password = secrets.token_hex(12).encode()
                def expect(needle):
                    out = g.until(needle,timeout=18 if needle==b"Username: " else 40)
                    if password in out: raise AssertionError("password echoed")
                    print(name,needle,out[-250:],flush=True)
                g.send(b"passwd lixin\r")
                expect(b"New password: ")
                g.send(password+b"\r")
                expect(b"Retype new password: ")
                g.send(password+b"\r")
                expect(b"Password updated")
                expect(b"$ ")
                for good in (False, True):
                    if args.gdm:
                        g.send(b"gdm --console\r")
                        expect(b"Username: ")
                        g.send(b"lixin\r")
                    else:
                        g.send(b"login lixin\r")
                    expect(b"Password:")
                    g.send((password if good else b"invalid")+b"\r")
                    if good:
                        expect(b"Starting session for lixin (uid 1000)")
                        expect(b"$ ")
                        g.send(b"pwd\r")
                        expect(b"/home/lixin")
                        expect(b"$ ")
                        g.send(b"echo private > /check-denied\r")
                        expect(b"$ ")
                        g.send(b"exit\r")
                    else:
                        expect(b"Login failed")
                    expect(b"$ ")
                g.send(b"cat /check-denied\r")
                out=g.until(b"$ ")
                assert b"\nprivate\n" not in out
                print(name,"password provisioned, invalid credential refused, user shell entered its home",flush=True)
                h["native"]["shutdown"](g)


if __name__=="__main__":
    main()
