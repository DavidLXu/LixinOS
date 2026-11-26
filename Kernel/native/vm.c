/* Sparse, process-private AArch64 page tables. A 2 MiB span obtains backing on
 * its first accessible mapping. Each exposed 4 KiB page is cleared separately.
 * PROT_NONE reservations need page tables only. Unmap returns empty spans. */
#include "vm.h"
typedef unsigned long word;
extern void *memset(void *, int, word);
#define PAGE 4096UL
#define BLOCK (512 * PAGE)
#define TOTAL (LX_VM_SIZE / PAGE)
extern void *memcpy(void *, const void *, word);
#define FILE_PAGE (1UL << 56)
#define EOF_PAGE FILE_PAGE
#define ABSENT (1UL << 55)
#define PA_MASK 0x0000fffffffff000UL
#define FIXED 0x10UL
#define NOREPLACE 0x100000UL

static word *leaf(const struct vm_space *s, word index) {
    word *dir = s->dirs[index >> 18];
    return dir ? (word *)(dir[(index >> 9) & 511] & PA_MASK) : 0;
}
static word perm(word prot) {
    if (!prot) return ABSENT;
    return 3 | (1UL << 10) | (3UL << 8) | (1UL << 6) | (1UL << 53) |
           (prot & 2 ? 0 : 1UL << 7) | (prot & 4 ? 0 : 1UL << 54);
}
static int used(const word *entries) {
    for (unsigned i = 0; i < 512; ++i) if (entries[i]) return 1;
    return 0;
}
static word next_run(const struct vm_space *s, word start, word end, word need) {
    word run = 0;
    while (start < end) {
        word *table = leaf(s, start);
        word n = 512 - (start & 511);
        if (n > end - start) n = end - start;
        if (!table) {
            run += n;
            start += n;
            if (run >= need) return start - run;
            continue;
        }
        for (; n; --n, ++start) {
            run = table[start & 511] ? 0 : run + 1;
            if (run == need) return start + 1 - need;
        }
    }
    return TOTAL;
}
/* A page with bit one set keeps its EL0 access flags while invalid. ZERO
 * denotes a newly mapped page, DEFER also covers preserved swap contents. */
#define DEFER (1UL << 57)
#define ZERO_PAGE (1UL << 58)
#define DISK_FRAME 2UL
#define DIR_BYTES (3 * PAGE)
static word *directories;
static unsigned residents, victim_hint;
static const struct vm_space *pinned;
static word pin_first, pin_end;
extern word *vm_current_root(void);

static int protected_span(word *dir, unsigned slot) {
    if (!pinned) return 0;
    for (unsigned i = pin_first >> 18; i <= (pin_end-1) >> 18; ++i) {
        if (pinned->dirs[i] == dir) {
            word page = ((word)i << 18) + (word)slot*512;
            return page < pin_end && page+512 > pin_first;
        }
    }
    return 0;
}
static void unlink_directory(word *dir) {
    word **link = &directories;
    while (*link && *link != dir) link = (word **)&(*link)[1024];
    if (*link) *link = (word *)dir[1024];
}
static void free_frame(word frame) {
    if (!frame) return;
    if (frame & DISK_FRAME) vm_swap_discard(frame >> 12);
    else { vm_release((void *)(frame & PA_MASK), BLOCK); --residents; }
}
static void restore_entries(word *table, word frame) {
    for (unsigned i=0; i<512; ++i) {
        word entry = table[i];
        if (!(entry & DEFER) || (entry & ZERO_PAGE)) continue;
        word pa = frame + i*PAGE;
        if ((entry & 2) && !(entry & (1UL<<54))) vm_sync_code(pa);
        table[i] = pa | (entry & ~DEFER) | ((entry >> 1) & 1);
    }
    vm_sync();
}
static word reclaim_frame(void) {
    if (!vm_swap_free()) return 0;
    for (word *dir=directories; dir; dir=(word *)dir[1024]) {
        for (unsigned i=0; i<512; ++i) {
            unsigned slot = (victim_hint+i)&511;
            word frame = dir[512+slot];
            if (!frame || (frame & 3) || protected_span(dir,slot)) continue;
            word *table = (word *)(dir[slot] & PA_MASK);
            for (unsigned j=0; j<512; ++j) {
                word entry = table[j];
                if (entry & PA_MASK) table[j] = (entry & ~(PA_MASK|1UL)) | DEFER;
            }
            vm_sync();
            unsigned id = vm_swap_save((void *)frame);
            if (!id) { restore_entries(table,frame); return 0; }
            dir[512+slot] = ((word)id<<12) | DISK_FRAME;
            victim_hint = slot+1;
            return frame;
        }
    }
    return 0;
}
static word obtain_frame(void) {
    word pa = 0;
    unsigned limit = vm_swap_limit();
    if (vm_swap_total() && ((limit && residents >= limit) || vm_memory_low()))
        pa = reclaim_frame();
    if (!pa) {
        pa = (word)vm_allocate(BLOCK);
        if (pa) ++residents;
        else pa = reclaim_frame();
    }
    return pa;
}
static word resident_frame(word *dir, unsigned slot) {
    word old = dir[512+slot];
    if (old && !(old & DISK_FRAME)) return old & PA_MASK;
    word frame = obtain_frame();
    if (!frame) return 0;
    if (old) {
        unsigned id = old >> 12;
        if (!vm_swap_load(id,(void *)frame)) { free_frame(frame); return 0; }
        vm_swap_discard(id);
        restore_entries((word *)(dir[slot] & PA_MASK),frame);
    }
    dir[512+slot] = frame;
    return frame;
}
static int fault_page(word *dir, word page, int write, int execute) {
    if (!dir) return 0;
    unsigned slot = (page>>9)&511;
    word *table = (word *)(dir[slot] & PA_MASK);
    if (!table) return 0;
    word *pte = &table[page&511], entry = *pte;
    if (!(entry & 2) || (write && (entry & 128)) ||
        (execute && (entry & (1UL<<54)))) return 0;
    if (entry & 1) return 1;
    if (!(entry & DEFER)) return 0;
    word frame = resident_frame(dir,slot);
    if (!frame) return 0;
    if (entry & ZERO_PAGE) {
        word pa = frame+(page&511)*PAGE;
        memset((void *)pa,0,PAGE);
        if (!(entry & (1UL<<54))) vm_sync_code(pa);
        *pte = pa | (entry & ~(ZERO_PAGE|DEFER)) | 1;
        vm_sync();
    }
    return (*pte & 1) != 0;
}
int vm_touch(word address, int write, int execute) {
    if (address < LX_VM_BASE || address-LX_VM_BASE >= LX_VM_SIZE) return 0;
    word *root=vm_current_root();
    word *dir=(word *)(root[address>>30] & PA_MASK);
    return fault_page(dir,(address-LX_VM_BASE)/PAGE,write,execute);
}
int vm_handle_fault(word syndrome, word address) {
    unsigned ec=syndrome>>26;
    if ((ec!=0x20 && ec!=0x24) || (syndrome & 60)!=4) return 0;
    return vm_touch(address,ec==0x24 && (syndrome&64),ec==0x20);
}
static void pin(const struct vm_space *s, word first, word end) {
    pinned=s; pin_first=first; pin_end=end;
}
/* No physical page is recycled until both cores have discarded translations. */
static void prune(struct vm_space *s, unsigned bank, word first, word end) {
    for (word idx = first & ~511UL; idx < end; idx += 512) {
        word *dir = s->dirs[idx >> 18];
        if (!dir) continue;
        word slot = (idx >> 9) & 511;
        word *table = leaf(s, idx);
        if (table && !used(table)) {
            dir[slot] = 0;
            vm_sync();
            vm_release(table, PAGE);
            free_frame(dir[512 + slot]);
            dir[512 + slot] = 0;
        }
        if (!used(dir)) {
            s->dirs[idx >> 18] = 0;
            vm_publish(bank, s);
            vm_sync();
            unlink_directory(dir);
            vm_release(dir, DIR_BYTES);
        }
    }
}
static int prepare(struct vm_space *s, unsigned bank, word first, word end, word prot) {
    for (word idx = first & ~511UL; idx < end; idx += 512) {
        word slot = (idx >> 9) & 511;
        word *dir = s->dirs[idx >> 18];
        if (!dir) {
            dir = vm_allocate(DIR_BYTES);
            if (!dir) return 0;
            memset(dir, 0, DIR_BYTES);
            dir[1024]=(word)directories; directories=dir;
            s->dirs[idx >> 18] = dir;
            vm_publish(bank, s);
        }
        if (!dir[slot]) {
            void *table = vm_allocate(PAGE);
            if (!table) return 0;
            memset(table, 0, PAGE);
            dir[slot] = (word)table | 3;
        }
        if (prot && (dir[512+slot] & DISK_FRAME) && !resident_frame(dir,slot)) return 0;
        if (prot && !dir[512 + slot]) {
            void *backing = (void *)obtain_frame();
            if (!backing) return 0;
            dir[512 + slot] = (word)backing | 1; /* unpublished until the entire request fits */
        }
    }
    return 1;
}
/* Invalidate anonymous contents without changing the reservation or access
 * rights. File snapshots cannot be reconstructed from an anonymous zero page. */
static long discard(struct vm_space *space, word first, word end) {
    for (word page=first; page<end; ++page) {
        word *table=leaf(space,page);
        if (!table || !table[page&511]) return -12;
        if (table[page&511] & FILE_PAGE) return -95;
    }
    for (word page=first; page<end; ++page) {
        word *pte=leaf(space,page)+(page&511);
        *pte=(*pte & ~(PA_MASK|1UL)) | DEFER | ZERO_PAGE;
    }
    vm_sync();
    for (word page=first & ~511UL; page<end; page+=512) {
        word *dir=space->dirs[page>>18];
        word *backing=dir+512+((page>>9)&511), *table=leaf(space,page);
        if (!*backing) continue;
        unsigned i;
        for (i=0;i<512;++i)
            if ((table[i]&PA_MASK) || ((table[i]&DEFER) && !(table[i]&ZERO_PAGE))) break;
        if (i==512) { free_frame(*backing); *backing=0; }
    }
    return 0;
}
long vm_call(struct vm_space *s, unsigned bank, word op, const word *args,
             const unsigned char *file, word size) {
    if (op == LX_VM_QUERY) return 1;
    word addr = args[0], length = args[1], prot = args[2], flags = args[3];
    int from_file = op == LX_VM_FILE;
    word offset = args[5];
    if (from_file) {
        if ((offset & (PAGE-1)) || offset > (~0UL >> 1)) return -22;
        op = LX_VM_MAP;
    }
    if ((op > LX_VM_UNMAP && op != LX_VM_DISCARD) || !length || length > LX_VM_SIZE) return -22;
    if (op != LX_VM_UNMAP && (prot & ~7UL)) return -22;
    if (op != LX_VM_UNMAP && (prot & 6) == 6) return -13;
    word n = (length + PAGE - 1) / PAGE;
    int inside = addr >= LX_VM_BASE && addr - LX_VM_BASE <= LX_VM_SIZE - n * PAGE;
    word first = inside ? (addr - LX_VM_BASE) / PAGE : TOTAL;
    if (op == LX_VM_MAP && !(flags & (FIXED | NOREPLACE))) {
        if (!inside || next_run(s, first, first+n, n) == TOTAL) {
            first = next_run(s, s->hint, TOTAL, n);
            if (first == TOTAL) first = next_run(s, 0, TOTAL, n);
        }
        if (first == TOTAL) return -12;
    } else if (!inside || (addr & (PAGE - 1))) return -22;
    word end = first + n;
    if (op == LX_VM_DISCARD) return discard(s,first,end);
    if (op == LX_VM_PROTECT || (op == LX_VM_MAP && (flags & NOREPLACE))) {
        for (word i = first; i < end; ++i) {
            word *t = leaf(s, i);
            int exists = t && t[i & 511];
            if (!exists && op == LX_VM_PROTECT) return -12;
            if (exists && op == LX_VM_MAP) return -17;
        }
    }
    if (op != LX_VM_UNMAP) {
        pin(s,first,end);
        int ready = prepare(s, bank, first, end, from_file || (prot && !vm_swap_total()));
        pinned = 0;
        for (word i = first & ~511UL; i < end; i += 512) {
            word *dir = s->dirs[i >> 18];
            if (!dir) continue;
            word *backing = &dir[512 + ((i >> 9) & 511)];
            if (*backing & 1) {
                *backing &= ~1UL;
                if (!ready) { free_frame(*backing); *backing = 0; }
            }
        }
        if (!ready) { prune(s, bank, first, end); return -12; }
    }
    /* Break before make when replacing existing valid descriptors. Keep the
     * PA in an invalid software entry across the TLB invalidation. */
    for (word i = first; i < end; ++i) {
        word *t = leaf(s, i);
        if (t && (t[i & 511] & 1)) t[i & 511] = (t[i & 511] & (PA_MASK|FILE_PAGE)) | ABSENT;
    }
    vm_sync();
    for (word i = first; i < end; ++i) {
        word *t = leaf(s, i);
        if (!t) continue;
        word *pte = t + (i & 511);
        if (op == LX_VM_UNMAP) { *pte = 0; continue; }
        if (op == LX_VM_PROTECT && *pte == EOF_PAGE) continue;
        word file_at = offset + (i-first) * PAGE;
        if (from_file && file_at >= size) { *pte = EOF_PAGE; continue; }
        word file_flag = from_file ? FILE_PAGE : op == LX_VM_PROTECT ? *pte & FILE_PAGE : 0;
        word pa = op == LX_VM_PROTECT ? *pte & PA_MASK : 0;
        if (vm_swap_total() && !from_file && !pa) {
            word saved = op == LX_VM_PROTECT && (*pte & DEFER) ? *pte & ZERO_PAGE : ZERO_PAGE;
            *pte = file_flag | DEFER | saved | (perm(prot) & ~1UL);
            continue;
        }
        if ((prot || from_file) && !pa) {
            pa = s->dirs[i >> 18][512 + ((i >> 9) & 511)] + (i & 511) * PAGE;
            memset((void *)pa, 0, PAGE);
            if (from_file) {
                word count = size - file_at;
                memcpy((void *)pa, file + file_at, count < PAGE ? count : PAGE);
            }
        }
        if (prot & 4) vm_sync_code(pa);
        *pte = file_flag | pa | perm(prot);
    }
    vm_sync();
    if (op == LX_VM_MAP) {
        s->hint = end;
        return LX_VM_BASE + first * PAGE;
    }
    if (op == LX_VM_UNMAP) {
        prune(s, bank, first, end);
        if (first < s->hint) s->hint = first;
    }
    return 0;
}
int vm_readable(const struct vm_space *s, word addr, word len) {
    if (addr < LX_VM_BASE || addr - LX_VM_BASE >= LX_VM_SIZE ||
        len > LX_VM_SIZE - (addr - LX_VM_BASE)) return 0;
    if (!len) return 1;
    word first = (addr-LX_VM_BASE)/PAGE, end = (addr-LX_VM_BASE+len+PAGE-1)/PAGE;
    pin(s,first,end);
    for (word page=first; page<end; ++page) {
        word *t = leaf(s,page);
        if (!t || (!(t[page&511]&1) && !fault_page(s->dirs[page>>18],page,0,0))) {
            pinned=0;
            return 0;
        }
    }
    pinned=0;
    return 1;
}
/* The caller detaches all directory entries and invalidates before teardown. */
void vm_destroy(struct vm_space *s) {
    for (unsigned d = 0; d < LX_VM_DIRS; ++d) {
        word *dir = s->dirs[d];
        if (!dir) continue;
        for (unsigned i = 0; i < 512; ++i) {
            if (dir[i]) vm_release((void *)(dir[i] & PA_MASK), PAGE);
            free_frame(dir[512+i]);
        }
        unlink_directory(dir);
        vm_release(dir, DIR_BYTES);
        s->dirs[d] = 0;
    }
    s->hint = 0;
}
