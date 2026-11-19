#include "native.h"

/* QEMU boots this small ELF once per power cycle. Disk snapshots and ELF
 * segments are fully checked before changing any byte in the load region. */
static u32 word(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u64 wide(const u8 *p) { return word(p) | (u64)word(p + 4) << 32; }
static u64 load(struct file f) {
    const u8 *elf = f.data;
    if (f.size < 64 || f.size > (1u << 20) || word(elf) != 0x464c457f || elf[4] != 2 ||
        elf[5] != 1 || elf[6] != 1 || word(elf + 16) != (183u << 16 | 2) || word(elf + 20) != 1 ||
        word(elf + 52) != (56u << 16 | 64))
        return 0;
    u64 entry = wide(elf + 24), phoff = wide(elf + 32);
    unsigned count = elf[56] + 256u * elf[57];
    if (!count || phoff > f.size || count > (f.size - phoff) / 56 || (entry & 3))
        return 0;
    int found = 0;
    u64 previous_end = 0x40200000;
    for (unsigned i = 0; i < count; i++) {
        const u8 *ph = elf + phoff + 56 * i;
        if (word(ph) != 1)
            continue;
        u32 flags = word(ph + 4);
        u64 off = wide(ph + 8), addr = wide(ph + 24), file = wide(ph + 32), mem = wide(ph + 40);
        if (off > f.size || file > f.size - off || file > mem || addr != wide(ph + 16) ||
            addr < previous_end || addr >= 0x44000000 || mem > 0x44000000UL - addr)
            return 0;
        previous_end = addr + mem;
        if ((flags & 1) && file >= 4 && entry >= addr && entry - addr <= file - 4)
            found = 1;
    }
    if (!found)
        return 0;
    for (unsigned i = 0; i < count; i++) {
        const u8 *ph = elf + phoff + 56 * i;
        if (word(ph) != 1)
            continue;
        u8 *dest = (u8 *)wide(ph + 24);
        u64 size = wide(ph + 32);
        memcpy(dest, elf + wide(ph + 8), size);
        memset(dest + size, 0, wide(ph + 40) - size);
    }
    // No caches or MMU were enabled by the bootstrap. Synchronize subsequent
    // instruction fetches before transferring control to the new entry point.
    asm volatile("dsb sy\n.long 0xd508751f\ndsb sy\nisb" ::: "memory"); // ic iallu
    return entry;
}
void kernel_main(void) {
    struct file f;
    if (mount_disk() && lookup("/boot/kernel.elf", &f)) {
        u64 entry = load(f);
        if (entry)
            ((void (*)(void))entry)();
    }
    volatile u32 *uart = (volatile u32 *)0x09000000;
    uart[0x2c / 4] = 3 << 5;
    uart[0x30 / 4] = 0x301;
    const char *message = "LixinOS boot: cannot load /boot/kernel.elf\r\n";
    while (*message) {
        while (uart[0x18 / 4] & (1 << 5)) {
        }
        uart[0] = *message++;
    }
    power_off();
    for (;;) {
    }
}
