"""Run the upstream MuJoCo pendulum trajectory on both guest kernels."""
import math
import re
import runpy
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
BOOT = runpy.run_path(str(HERE / "test-user.py"))["boot"]
INSTALL = runpy.run_path(str(ROOT / "User/gcc/install.py"))["install"]
# Recorded with the official macOS MuJoCo 4.0.1 library, RK4, 2 ms.
REFERENCE = [(0, 0., 0.5, 0.), (500, 1., -0.040785948333, 2.319938370319),
             (1000, 2., -0.493483430325, -0.370769987216)]


def main():
    release = subprocess.check_output(["python3", HERE / "kernel-release.py", "latest"], text=True).strip()
    with tempfile.TemporaryDirectory() as tmp:
        disk = Path(tmp) / "test.img"
        subprocess.run(["cp", "-c", ROOT / "data/desktop-pingfang.img", disk], check=True)
        INSTALL(ROOT / "Kernel/target/user-build/mujoco-package", disk, "mujoco")
        for kernel in ("rust", "tcc"):
            launch = ["env", f"LIXINOS_TCC_DISK={disk}", HERE / "run-kernel.sh",
                      "--serial", "--kernel", kernel, "--release", release]
            with BOOT(disk, launch, prompt=b"$ ") as guest:
                guest.send(b"mujoco\r")
                output = guest.until(b"$ ", timeout=60).decode(errors="replace")
                print(kernel, release, output, flush=True)
                assert "MuJoCo 4.0.1 nq=1 nv=1" in output
                rows = re.findall(r"step=(\d+) time=([\d.]+) q0=([-\d.]+) v0=([-\d.]+)", output)
                assert len(rows) == len(REFERENCE)
                for actual, expected in zip(rows, REFERENCE):
                    assert all(math.isclose(float(x), y, abs_tol=1e-9, rel_tol=0)
                               for x, y in zip(actual, expected)), (actual, expected)
                guest.expect(b"echo $?\r", b"$ ", b"\n0\n")
                guest.send(b"shutdown\r")
                assert guest.vm.wait(timeout=15) == 0


if __name__ == "__main__":
    main()
