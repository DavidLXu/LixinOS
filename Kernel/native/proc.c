#include "native.h"
#include "version.h"
#include "smp.h"

/* A physical /proc on an older disk takes precedence over the virtual mount. */
static const char *names[] = {"cpuinfo", "meminfo", "stat", "uptime", "version"};
static int compare(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (u8)*a - (u8)*b;
}
static int mounted;
void proc_init(void) {
    struct file f;
    mounted = !lookup("/proc", &f);
}
int proc_path(const char *path) {
    return mounted && path[0] == '/' && path[1] == 'p' && path[2] == 'r' && path[3] == 'o' &&
           path[4] == 'c' && (!path[5] || path[5] == '/');
}
static char *seconds(char *p, u64 centis) {
    p = add_number(p, centis / 100);
    *p++ = '.';
    *p++ = '0' + centis / 10 % 10;
    *p++ = '0' + centis % 10;
    *p = 0;
    return p;
}
int vfs_lookup(const char *path, struct file *f) {
    if (!proc_path(path))
        return lookup(path, f);
    if (!path[5]) {
        *f = (struct file){0, 0xffffffff};
        return 1;
    }
    unsigned i = 0;
    while (i < 5 && compare(names[i], path + 6))
        i++;
    if (i == 5)
        return 0;
    static char buffer[NATIVE_TASKS][512];
    char *text = buffer[task_active()], *p = text;
    if (i == 0)
        for (unsigned cpu = 0; cpu < smp_cpus(); cpu++) {
            p = add_number(add_text(p, "processor\t: "), cpu);
            p = add_text(p, "\nmodel name\t: LixinOS aarch64 vCPU\n");
        }
    if (i == 1) {
        u64 free = (heap_capacity_bytes() - heap_bytes()) / 1024;
        p = add_number(add_text(p, "MemTotal:       "), native_ram_bytes() / 1024);
        p = add_number(add_text(p, " kB\nMemFree:        "), free);
        p = add_number(add_text(p, " kB\nMemAvailable:   "), free);
        p = add_text(p, " kB\nSwapTotal:      0 kB\nSwapFree:       0 kB\n");
    }
    if (i == 2 || i == 3) {
        u64 hz = clock_hz(), time = clock_ticks() * 100 / hz, idle = idle_ticks() * 100 / hz;
        if (i == 2) {
            u64 remote = smp_busy_ticks() * 100 / hz;
            u64 other_idle = smp_cpus() == 2 && time > remote ? time - remote : 0;
            for (unsigned cpu = 0; cpu <= smp_cpus(); cpu++) {
                p = add_number(add_text(p, cpu == 2 ? "cpu1 0 0 " : cpu ? "cpu0 0 0 " : "cpu  0 0 "),
                               cpu == 2 ? remote : time - idle + (cpu ? 0 : remote));
                *p++ = ' ';
                p = add_number(p, cpu == 2 ? other_idle : idle + (cpu ? 0 : other_idle));
                p = add_text(p, " 0 0 0 0 0 0\n");
            }
        } else {
            p = seconds(p, time);
            *p++ = ' ';
            p = seconds(p, idle);
            p = add_text(p, "\n");
        }
    }
    if (i == 4)
        p = add_text(p, KERNEL_SUMMARY "\n");
    *f = (struct file){(const u8 *)text, p - text};
    return 1;
}
long vfs_entry(const char *path, u64 index, char *name) {
    if (proc_path(path)) {
        if (path[5] || index >= 5)
            return -1;
        add_text(name, names[index]);
        return 0;
    }
    if (!mounted || compare(path, "/"))
        return directory_entry(path, index, name);
    long size = directory_entry(path, index, name);
    if (size != -1 && compare(name, "proc") < 0)
        return size;
    if (index && directory_entry(path, index - 1, name) != -1 && compare(name, "proc") > 0)
        return directory_entry(path, index - 1, name);
    if (size == -1 && index && directory_entry(path, index - 1, name) == -1)
        return -1;
    add_text(name, "proc");
    return -2;
}
