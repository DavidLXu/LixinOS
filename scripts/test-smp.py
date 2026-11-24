"""Verify independent EL0 execution, both timer controllers and the one-vCPU fallback."""
import argparse
from pathlib import Path
import re
import runpy
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
BOOT = runpy.run_path(str(HERE / "test-user.py"))["boot"]

# The recurrence has a fixed result modulo 2^32. Separate processes must retain
# their own globals and integer state across IRQs and kernel entry.
PROGRAM = """
#include <lixin.h>
#include <stdio.h>
#include "/src/console.h"
static unsigned value = 19;
static char *child[] = {"bench", "child"};
int main(int argc, char **argv) {
    if (argc > 1) {
        long previous = 0;
        for (int part = 0; part < (CLOCK_LOAD ? 900 : 9); part++) {
            for (unsigned i = 0; i < (CLOCK_LOAD ? 200000u : 20000000u); i++) value = value * 1664525u + 1013904223u;
            if (!CLOCK_LOAD) { if (lx_pid() <= 0) return 7; continue; }
            long us = console_call(38, 1);
            if (us < previous || console_call(38, 0) < us / 1000) return 7;
            if (console_call(38, 2) <= 0 || console_call(38, 3) != -1) return 9;
            previous = us;
        }
        return lx_pid() > 0 && value == 3872163091u ? 33 : 8;
    }
    for (int concurrent = 0; concurrent <= 1; concurrent++) {
        long start = console_call(38, 0);
        int a = lx_spawn_async("/bin/smpbench", 2, child);
        if (a < 0 || (!concurrent && lx_wait(a) != 33)) return 2;
        int b = lx_spawn_async("/bin/smpbench", 2, child);
        if (b < 0 || (concurrent && lx_wait(a) != 33) || lx_wait(b) != 33 || value != 19) return 3;
        printf("batch %d ms %ld\\n", concurrent, console_call(38, 0) - start);
    }
    return 0;
}
"""


# The writer observes the reader sleeping before it sends any bytes.
# The payload exceeds both pipe capacities and ends with a writer-close EOF.
PIPE_PROGRAM = r"""
#include <lixin.h>
#include <stdio.h>
#include "/src/console.h"
#define TOTAL 16385
static unsigned char buffer[1024];
static int waiting(int pid) {
    char *row = (char *)0x443d7000;
    long n = console_call(29, 6);
    if (n <= 0 || n >= 4096) return 0;
    char *limit = row + n;
    while (row < limit) {
        int id = 0;
        while (row < limit && *row >= 48 && *row <= 57) id = 10 * id + *row++ - 48;
        if (id == pid && row < limit && *row++ == 32) {
            while (row < limit && *row >= 48 && *row <= 57) row++;
            if (row + 2 < limit && row[0] == 32 && row[1] == 119) return 1;
        }
        while (row < limit && *row++ != 10) {}
    }
    return 0;
}
int main(int argc, char **argv) {
    int cpu = console_call(29, 10);
    if (argc != 2 || cpu < 0 || cpu > 1) return 1;
    if (argv[1][0] == 119) {
        long deadline = console_call(38, 0) + 4000;
        int pid = 0;
        do {
            if (!pid) {
                int fd = lx_open("/pipe-reader");
                if (fd >= 0) { lx_read(fd, &pid, sizeof(pid)); lx_close(fd); }
            }
            if (pid && waiting(pid)) break;
            if (console_call(38, 0) >= deadline) return 2;
        } while (1);
        for (int pos = 0; pos < TOTAL;) {
            int n = TOTAL - pos;
            if (n > sizeof(buffer)) n = sizeof(buffer);
            for (int i = 0; i < n; i++) buffer[i] = pos+i ? (pos+i) % 251 : cpu;
            int sent = lx_write(1, buffer, n);
            if (sent <= 0) return 3;
            pos += sent;
        }
        return 0;
    }
    int id = lx_pid();
    if (lx_save_file("/pipe-reader", &id, sizeof(id)) < 0) return 4;
    int writer = -1, pos = 0, got;
    while ((got = lx_read(0, buffer, sizeof(buffer))) > 0) {
        for (int i = 0; i < got; i++, pos++) {
            if (!pos) writer = buffer[i];
            else if (buffer[i] != pos % 251) return 5;
        }
    }
    if (got < 0 || pos != TOTAL) return 6;
    printf("pipe reader %d writer %d bytes %d EOF\n", cpu, writer, pos);
    return 0;
}
"""

def exercise(kernel, flavor, disk, cpus, machine, gcc, clock_load):
    ram = ("24G" if flavor == "tcc" else "12G") if disk.stat().st_size > 5 << 30 else (
        "1536M" if flavor == "tcc" else "640M")
    args = ["qemu-system-aarch64", "-machine", machine, "-accel", "hvf", "-cpu", "host",
            "-smp", str(cpus), "-m", ram, "-kernel", str(kernel), "-display", "none",
            "-monitor", "none", "-serial", "stdio", "-nic", "none", "-global",
            "virtio-mmio.force-legacy=false", "-drive", f"if=none,id=fs,format=raw,file={disk}",
            "-device", "virtio-blk-device,drive=fs,bus=virtio-mmio-bus.0",
            "-object", "rng-random,id=rng,filename=/dev/urandom",
            "-device", "virtio-rng-device,rng=rng,bus=virtio-mmio-bus.5"]
    with BOOT(disk, args, prompt=b"LixinOS Bash (C shell)") as guest:
        guest.until(b"$ ")

        def command(line):
            guest.send(line.encode() + b"\r")
            return guest.until(b"$ ", timeout=100)

        assert f"vcpus={cpus}\n".encode() in command("cpuinfo")
        assert command("cat /proc/cpuinfo").count(b"processor\t:") == cpus
        program = f"#define CLOCK_LOAD {int(clock_load)}\n" + PROGRAM.strip()
        for i, line in enumerate(program.splitlines()):
            redirect = ">" if i == 0 else ">>"
            command(f"echo '{line}' {redirect} /smpbench.c")
        command("tcc /smpbench.c -o /bin/smpbench")
        for i, line in enumerate(PIPE_PROGRAM.strip().splitlines()):
            command(f"echo '{line}' {'>' if i == 0 else '>>'} /pipeprobe.c")
        output = command("tcc /pipeprobe.c -o /bin/pipeprobe")
        assert b"[exit" not in output, output
        output = command("/bin/pipeprobe write | /bin/pipeprobe read")
        result = re.search(rb"^pipe reader ([01]) writer ([01]) bytes 16385 EOF$", output, re.M)
        assert result and ((result[1] != result[2]) == (cpus == 2)), output
        print(f"{flavor}, {cpus} CPU: {result[0].decode()}", flush=True)
        times = []
        for _ in range(3):
            output = command("/bin/smpbench")
            ms = re.findall(rb"^batch [01] ms (\d+)$", output, re.M)
            assert len(ms) == 2 and b"[exit" not in output, output
            times.append([int(x) for x in ms])
        stat = command("cat /proc/stat")
        secondary = re.search(rb"^cpu1 0 0 (\d+)", stat, re.M)
        if cpus == 2:
            assert secondary and int(secondary[1]) > 0, stat
        else:
            assert secondary is None, stat
        if gcc:
            # Both drivers create an assembler temporary within the same second.
            command("echo 'int square(int x){return x*x;}' > /unit.c")
            source = [
                "#include <lixin.h>",
                "int main(void){char *a[]={\"gcc\",\"-O2\",\"-c\",\"/unit.c\",\"-o\",\"/a.o\"};",
                "int x=lx_spawn_async(\"/bin/gcc\",6,a);a[5]=\"/b.o\";",
                "int y=lx_spawn_async(\"/bin/gcc\",6,a);",
                "int first=lx_wait(x), second=lx_wait(y); return first || second;}",
            ]
            for i, line in enumerate(source):
                command(f"echo '{line}' {'>' if i == 0 else '>>'} /compile.c")
            command("tcc /compile.c -o /compile")
            for _ in range(3):
                assert b"[exit" not in command("/compile")
                assert b"[exit" not in command("cmp /a.o /b.o")
        # Receive foreground input while a compute-only child is still alive.
        command("/bin/smpbench &")
        guest.send(b"echo foreground-ready\r")
        before = guest.until(b"\nforeground-ready\r\n")
        assert b"batch 1 ms" not in before
        guest.until(b"batch 1 ms", timeout=30)
        guest.send(b"q\r")
        assert guest.vm.wait(timeout=45) == 0
    print(f"{flavor}, {machine}, {cpus} CPU: serial/parallel pairs in ms: {times}", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", required=True, type=Path, help="bootable kernel ELF")
    parser.add_argument("--disk", required=True, type=Path, help="idle initialized source disk")
    parser.add_argument("--kernel", required=True, choices=("rust", "tcc"))
    parser.add_argument("--cpus", type=int, choices=(1, 2), default=2)
    parser.add_argument("--gic3", action="store_true")
    parser.add_argument("--clock", action="store_true", help="exercise frequent monotonic clock calls during each compute batch")
    parser.add_argument("--gcc", action="store_true", help="also test two installed GCC drivers")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        disk = Path(tmp) / "test.img"
        subprocess.run(["cp", "-c", args.disk, disk], check=True)
        exercise(args.image, args.kernel, disk, args.cpus, "virt,gic-version=3" if args.gic3 else "virt", args.gcc, args.clock)
