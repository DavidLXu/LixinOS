# MuJoCo on LixinOS

The AArch64 port runs the MuJoCo 4.0.1 CPU dynamics engine and libccd. It uses
binary models compiled by the same MuJoCo version. The guest package provides
a shell runner and a passive pendulum. The Physics Win3 package also links the
engine and embeds a three-link model.

In the guest shell, run:

```sh
mujoco
mujoco /lib/mujoco/pendulum.mjb 500
```

The default run takes 1,000 steps of 2 ms, starting at the first keyframe if
one exists. It prints the first generalized position and velocity at the
start, midpoint and end. Use standalone MJB models with embedded assets and
built-in MuJoCo components. MJCF compilation takes place on the host.
Dynamic plugins, Python bindings and the upstream 3D viewers are outside the
tested port.

In Desktop, open **物理仿真器 → 示例 → MuJoCo 三节起摆**. Press Space to run
or pause, **单步** to advance one 1/120-second control interval, and **R**
to reset. Choose **强化学习起摆** to run the same original policy on the
native solver. Each rod has length and mass 0.65 in SI units. The MuJoCo
version uses hinge coordinates, four RK4 steps per control interval and
torque limits of 16, 8 and 3 N·m. The UI reports the first simulation time at
which the rods have satisfied the balance criterion for two consecutive seconds.

## Build and verify

From the repository root on the development Mac:

```sh
bash User/physics/build.sh
python3 scripts/test-mujoco.py
```

Building Physics also builds the console runner. To build only the runner,
use `bash User/mujoco/build.sh`. The build verifies the pinned source archives
and reuses the AArch64 musl/C++ toolchain used by the GNOME port. The console
test boots both kernels on a disposable disk and compares the two-second
trajectory with the official macOS 4.0.1 library at three checkpoints.

To regenerate either checked-in MJB, build `compile-model.c` on the host
against MuJoCo 4.0.1 and run `./compile-model pendulum.xml pendulum.mjb` or
`./compile-model triple.xml triple.mjb`. To reproduce the reference
trajectory, also build `run.c` against the same host library and run
`./run pendulum.mjb`.

To install the packages into an offline disk:

```sh
python3 User/gcc/install.py --toolchain mujoco Kernel/target/user-build/mujoco-package /path/to/disk.img
python3 User/gcc/install.py --toolchain physics Kernel/target/user-build/physics-package /path/to/disk.img
```
