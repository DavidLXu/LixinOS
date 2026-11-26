/* Native AArch64 EL0 guest execution and sparse Copy-on-Write address spaces.
 * Shared identically between the Rust and self-hosted TCC kernels. */
#include "guest.h"
#include "kthread.h"
#include "smp.h"
#include "vm.h"
#include "ipc.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long word;

extern void *memset(void *, int, word);
extern void *memcpy(void *, const void *, word);
extern unsigned kt_slot(void);
extern int kt_access(word addr, int write);
extern void kt_service_enter(void);
extern void kt_service_leave(void);
extern word smp_ticks(void);
extern int smp_check_stop(void);
extern long guest_enter(struct lx_guest_ctx *shadow);
extern word guest_va_to_pa(word va, int write);
extern void guest_tlbi_va(word va);
extern void guest_clean_dcache(word addr, word len);
extern void guest_icache_sync(void);

#define PAGE          4096UL
#define BLOCK         (512UL * PAGE)
#define PA_MASK       0x0000fffffffff000UL
#define PTE_VALID     1UL
#define PTE_PAGE      3UL
#define PTE_EL0       (1UL << 6)
#define PTE_RO        (1UL << 7)
#define PTE_SH        (3UL << 8)
#define PTE_AF        (1UL << 10)
#define PTE_PXN       (1UL << 53)
#define PTE_UXN       (1UL << 54)
#define PTE_DEFER     (1UL << 55)
#define PTE_COW       (1UL << 56)
#define PTE_SHARED    (1UL << 57)
#define PTE_FILE      (1UL << 58)
#define DEVICE_L2_ATTR (1UL | (1UL << 10) | 4UL | (1UL << 53) | (1UL << 54))
#define DEVICE_L1_DEFAULT (1UL | (1UL << 10) | 4UL | (1UL << 53) | (1UL << 54))

#define LOW_L2_START  (LX_GUEST_LOW_BASE / BLOCK)   /* 96 */
#define HIGH_L1_BASE  (LX_GUEST_HIGH_BASE >> 30)    /* 128 */
#define HIGH_L1_COUNT ((LX_GUEST_HIGH_END - LX_GUEST_HIGH_BASE) >> 30) /* 384 */

/* 2 MiB slab -> 511 usable 4 KiB pages (page 0 holds slab metadata). */
#define MAX_SLABS     1024
#define MAX_PA_BLOCKS 16384 /* Covers up to 32 GiB physical address space. */
#define SLAB_PAGES    511

struct guest_slab {
    word base_pa;
    word free_bits[8];
    u16 refcount[512];
    u16 free_count;
    u16 index;
};

static struct guest_slab *slabs[MAX_SLABS];
static u16 pa_to_slab[MAX_PA_BLOCKS];
static unsigned slab_hint;

static unsigned ctz64(word x) {
    unsigned n = 0;
    if (!(x & 0xffffffffUL)) { n += 32; x >>= 32; }
    if (!(x & 0xffffUL))     { n += 16; x >>= 16; }
    if (!(x & 0xffUL))       { n += 8;  x >>= 8;  }
    if (!(x & 0xfUL))        { n += 4;  x >>= 4;  }
    if (!(x & 0x3UL))        { n += 2;  x >>= 2;  }
    if (!(x & 0x1UL))        { n += 1; }
    return n;
}

static struct guest_slab *slab_for_pa(word pa) {
    word blk = pa >> 21;
    if (blk >= MAX_PA_BLOCKS) return 0;
    u16 id = pa_to_slab[blk];
    if (!id) return 0;
    struct guest_slab *s = slabs[id - 1];
    if (!s || pa < s->base_pa || pa >= s->base_pa + BLOCK) return 0;
    return s;
}

static word alloc_page(int zero) {
    for (unsigned pass = 0; pass < 2; ++pass) {
        unsigned start = pass ? 0 : slab_hint;
        unsigned end = pass ? slab_hint : MAX_SLABS;
        for (unsigned i = start; i < end; ++i) {
            struct guest_slab *s = slabs[i];
            if (!s || !s->free_count) continue;
            for (unsigned w = 0; w < 8; ++w) {
                word bits = s->free_bits[w];
                if (!bits) continue;
                unsigned bit = ctz64(bits);
                unsigned idx = w * 64 + bit;
                s->free_bits[w] &= ~(1UL << bit);
                s->free_count--;
                s->refcount[idx] = 1;
                slab_hint = i;
                word pa = s->base_pa + (word)idx * PAGE;
                if (zero) memset((void *)pa, 0, PAGE);
                return pa;
            }
        }
    }
    /* Allocate a fresh 2 MiB slab. */
    unsigned slot = MAX_SLABS;
    for (unsigned i = 0; i < MAX_SLABS; ++i) {
        if (!slabs[i]) { slot = i; break; }
    }
    if (slot == MAX_SLABS) return 0;
    word base = (word)vm_allocate(BLOCK);
    if (!base) return 0;
    word blk = base >> 21;
    if ((base & (BLOCK - 1)) || blk >= MAX_PA_BLOCKS || pa_to_slab[blk]) {
        vm_release((void *)base, BLOCK);
        return 0;
    }
    struct guest_slab *s = (struct guest_slab *)base;
    memset(s, 0, sizeof(*s));
    s->base_pa = base;
    s->index = (u16)slot;
    s->free_count = SLAB_PAGES;
    for (unsigned w = 0; w < 8; ++w) s->free_bits[w] = ~0UL;
    s->free_bits[0] &= ~1UL; /* Page 0 holds this slab header. */
    slabs[slot] = s;
    pa_to_slab[blk] = (u16)(slot + 1);
    /* Return page 1 immediately. */
    s->free_bits[0] &= ~2UL;
    s->free_count--;
    s->refcount[1] = 1;
    slab_hint = slot;
    word pa = base + PAGE;
    if (zero) memset((void *)pa, 0, PAGE);
    return pa;
}

static void page_ref_inc(word pa) {
    struct guest_slab *s = slab_for_pa(pa);
    if (!s) return;
    unsigned idx = (pa - s->base_pa) >> 12;
    if (idx && idx < 512) s->refcount[idx]++;
}

static u16 page_ref_get(word pa) {
    struct guest_slab *s = slab_for_pa(pa);
    if (!s) return 0;
    unsigned idx = (pa - s->base_pa) >> 12;
    return (idx && idx < 512) ? s->refcount[idx] : 0;
}

static void page_ref_dec(word pa) {
    struct guest_slab *s = slab_for_pa(pa);
    if (!s) return;
    unsigned idx = (pa - s->base_pa) >> 12;
    if (!idx || idx >= 512 || !s->refcount[idx]) return;
    if (--s->refcount[idx]) return;
    s->free_bits[idx >> 6] |= 1UL << (idx & 63);
    s->free_count++;
    if (s->free_count == SLAB_PAGES) {
        word base = s->base_pa;
        unsigned slot = s->index;
        pa_to_slab[base >> 21] = 0;
        slabs[slot] = 0;
        vm_release((void *)base, BLOCK);
    }
}

/* Guest virtual address spaces (up to 64 across all processes). */
#define MAX_SPACES 64
#define MAX_BANKS  16

struct guest_space {
    int used;
    unsigned owner_bank;
    word *low_l2;       /* 4 KiB L2 table for L1 slot 0 (0..192MiB MMIO, 192MiB..1GiB guest) */
    word **high_l2;     /* 4 KiB page storing 384 L2 pointers for L1 slots 128..511 */
    word high_mask[6];  /* 384-bit mask of active L1 slots in high_l2 */
};

static struct guest_space spaces[MAX_SPACES];
static struct guest_space *bank_bound[MAX_BANKS];
static struct guest_space *cpu_bound[2];
static unsigned cpu_owner[2] = {MAX_BANKS, MAX_BANKS};

struct guest_task {
    word saved_sp;
    word deadline;
    int active;
    struct lx_guest_ctx shadow;
};

static struct guest_task gtasks[KT_TASK_SLOTS];
/* Read directly by guest-arch.S guest_current_ctx() on each core. */
struct lx_guest_ctx *guest_running[2];

static int valid_guest_range(word va, word len) {
    if (!len || (va & (PAGE - 1)) || (len & (PAGE - 1))) return 0;
    if (va >= LX_GUEST_LOW_BASE && va < LX_GUEST_LOW_END && len <= LX_GUEST_LOW_END - va)
        return 1;
    if (va >= LX_GUEST_HIGH_BASE && va < LX_GUEST_HIGH_END && len <= LX_GUEST_HIGH_END - va)
        return 1;
    return 0;
}

static word prot_bits(word prot) {
    word bits = PTE_PAGE | PTE_AF | PTE_SH | PTE_EL0 | PTE_PXN;
    if (!(prot & 2)) bits |= PTE_RO;
    if (!(prot & 4)) bits |= PTE_UXN;
    return bits;
}

static struct guest_space *get_space(unsigned bank, word id) {
    if (!id || id > MAX_SPACES) return 0;
    struct guest_space *s = &spaces[id - 1];
    return (s->used && s->owner_bank == bank) ? s : 0;
}

static void bind_space(unsigned bank, struct guest_space *next) {
    struct guest_space *prev = bank_bound[bank];
    if (prev == next) return;
    guest_publish_l1(bank, 0, next ? ((word)next->low_l2 | 3UL) : DEVICE_L1_DEFAULT);
    for (unsigned w = 0; w < 6; ++w) {
        word mask = (prev ? prev->high_mask[w] : 0) | (next ? next->high_mask[w] : 0);
        while (mask) {
            unsigned bit = 0;
            while (!((mask >> bit) & 1UL)) ++bit;
            mask &= ~(1UL << bit);
            unsigned idx = w * 64 + bit;
            word entry = (next && next->high_l2[idx]) ? ((word)next->high_l2[idx] | 3UL) : 0;
            guest_publish_l1(bank, HIGH_L1_BASE + idx, entry);
        }
    }
    bank_bound[bank] = next;
    for (unsigned c = 0; c < 2; ++c)
        if (cpu_owner[c] == bank) cpu_bound[c] = next;
    vm_sync();
}

word guest_get_bound(unsigned bank) {
    if (bank >= MAX_BANKS || !bank_bound[bank]) return 0;
    return (word)(bank_bound[bank] - spaces) + 1;
}

void guest_set_bound(unsigned bank, word space_id) {
    if (bank >= MAX_BANKS) return;
    bind_space(bank, space_id ? get_space(bank, space_id) : 0);
}

void guest_rebind_cpu(unsigned cpu, unsigned bank) {
    if (cpu >= 2) return;
    cpu_owner[cpu] = bank;
    struct guest_space *prev = cpu_bound[cpu];
    struct guest_space *next = bank < MAX_BANKS ? bank_bound[bank] : 0;
    if (prev == next) return;
    guest_publish_cpu_l1(cpu, 0, next ? ((word)next->low_l2 | 3UL) : DEVICE_L1_DEFAULT);
    for (unsigned w = 0; w < 6; ++w) {
        word mask = (prev ? prev->high_mask[w] : 0) | (next ? next->high_mask[w] : 0);
        while (mask) {
            unsigned bit = 0;
            while (!((mask >> bit) & 1UL)) ++bit;
            mask &= ~(1UL << bit);
            unsigned idx = w * 64 + bit;
            word entry = (next && next->high_l2[idx]) ? ((word)next->high_l2[idx] | 3UL) : 0;
            guest_publish_cpu_l1(cpu, HIGH_L1_BASE + idx, entry);
        }
    }
    cpu_bound[cpu] = next;
}

static word *ensure_l3(struct guest_space *s, unsigned bank, word va, int create) {
    word *l2;
    unsigned l2_idx;
    if (va < LX_GUEST_LOW_END) {
        l2 = s->low_l2;
        l2_idx = (va >> 21) & 511;
    } else {
        unsigned l1_idx = (va >> 30) - HIGH_L1_BASE;
        l2 = s->high_l2[l1_idx];
        if (!l2) {
            if (!create) return 0;
            word pa = alloc_page(1);
            if (!pa) return 0;
            l2 = (word *)pa;
            s->high_l2[l1_idx] = l2;
            s->high_mask[l1_idx >> 6] |= 1UL << (l1_idx & 63);
            if (bank_bound[bank] == s)
                guest_publish_l1(bank, HIGH_L1_BASE + l1_idx, pa | 3UL);
        }
        l2_idx = (va >> 21) & 511;
    }
    word entry = l2[l2_idx];
    if (!entry) {
        if (!create) return 0;
        word pa = alloc_page(1);
        if (!pa) return 0;
        l2[l2_idx] = pa | 3UL;
        return (word *)pa;
    }
    return (word *)(entry & PA_MASK);
}

static void release_pte(word pte) {
    if ((pte & PA_MASK) && !(pte & PTE_DEFER) && !(pte & PTE_SHARED))
        page_ref_dec(pte & PA_MASK);
}

static void flush_range(struct guest_space *s, unsigned bank, word va, word pages) {
    if (bank_bound[bank] != s) return;
    if (pages <= 32) {
        for (word i = 0; i < pages; ++i)
            guest_tlbi_va(va + i * PAGE);
    } else {
        vm_sync();
    }
}

static long space_create(unsigned bank) {
    unsigned id = 0;
    for (unsigned i = 0; i < MAX_SPACES; ++i) {
        if (!spaces[i].used) { id = i + 1; break; }
    }
    if (!id) return -12;
    word low_pa = alloc_page(1);
    word high_pa = alloc_page(1);
    if (!low_pa || !high_pa) {
        if (low_pa) page_ref_dec(low_pa);
        if (high_pa) page_ref_dec(high_pa);
        return -12;
    }
    word *low = (word *)low_pa;
    for (unsigned i = 0; i < LOW_L2_START; ++i)
        low[i] = ((word)i * BLOCK) | DEVICE_L2_ATTR;
    struct guest_space *s = &spaces[id - 1];
    memset(s, 0, sizeof(*s));
    s->used = 1;
    s->owner_bank = bank;
    s->low_l2 = low;
    s->high_l2 = (word **)high_pa;
    return (long)id;
}

static void destroy_l2_range(word *l2, unsigned first, unsigned end) {
    for (unsigned i = first; i < end; ++i) {
        if (!l2[i]) continue;
        word *l3 = (word *)(l2[i] & PA_MASK);
        for (unsigned j = 0; j < 512; ++j)
            if (l3[j]) release_pte(l3[j]);
        page_ref_dec((word)l3);
        l2[i] = 0;
    }
}

static void space_destroy_ptr(struct guest_space *s) {
    unsigned bank = s->owner_bank;
    if (bank_bound[bank] == s)
        bind_space(bank, 0);
    int cleared_cpu = 0;
    for (unsigned c = 0; c < 2; ++c)
        if (cpu_bound[c] == s) {
            guest_rebind_cpu(c, MAX_BANKS);
            cleared_cpu = 1;
        }
    if (cleared_cpu) vm_sync();
    destroy_l2_range(s->low_l2, LOW_L2_START, 512);
    for (unsigned w = 0; w < 6; ++w) {
        word mask = s->high_mask[w];
        while (mask) {
            unsigned bit = 0;
            while (!((mask >> bit) & 1UL)) ++bit;
            mask &= ~(1UL << bit);
            unsigned idx = w * 64 + bit;
            word *l2 = s->high_l2[idx];
            if (l2) {
                destroy_l2_range(l2, 0, 512);
                page_ref_dec((word)l2);
                s->high_l2[idx] = 0;
            }
        }
    }
    page_ref_dec((word)s->low_l2);
    page_ref_dec((word)s->high_l2);
    memset(s, 0, sizeof(*s));
}

static int clone_l2_range(word *src_l2, word *dst_l2, unsigned first, unsigned end, int *need_sync) {
    for (unsigned i = first; i < end; ++i) {
        if (!src_l2[i]) continue;
        word *src_l3 = (word *)(src_l2[i] & PA_MASK);
        word dst_pa = alloc_page(1);
        if (!dst_pa) return 0;
        word *dst_l3 = (word *)dst_pa;
        dst_l2[i] = dst_pa | 3UL;
        for (unsigned j = 0; j < 512; ++j) {
            word pte = src_l3[j];
            if (!pte) continue;
            if ((pte & PTE_DEFER) || (pte & PTE_SHARED)) {
                dst_l3[j] = pte;
                continue;
            }
            if (pte & PA_MASK) {
                page_ref_inc(pte & PA_MASK);
                if (!(pte & PTE_RO) || !(pte & PTE_COW)) {
                    if (!(pte & PTE_RO) && (pte & PTE_VALID)) *need_sync = 1;
                    pte |= PTE_RO | PTE_COW;
                    src_l3[j] = pte;
                }
                dst_l3[j] = pte;
            }
        }
    }
    return 1;
}

static long space_clone(unsigned bank, struct guest_space *src) {
    long dst_id = space_create(bank);
    if (dst_id < 0) return dst_id;
    struct guest_space *dst = &spaces[dst_id - 1];
    int need_sync = 0;
    if (!clone_l2_range(src->low_l2, dst->low_l2, LOW_L2_START, 512, &need_sync)) {
        space_destroy_ptr(dst);
        return -12;
    }
    for (unsigned w = 0; w < 6; ++w) {
        word mask = src->high_mask[w];
        while (mask) {
            unsigned bit = 0;
            while (!((mask >> bit) & 1UL)) ++bit;
            mask &= ~(1UL << bit);
            unsigned idx = w * 64 + bit;
            word *src_l2 = src->high_l2[idx];
            if (!src_l2) continue;
            word dst_l2_pa = alloc_page(1);
            if (!dst_l2_pa) {
                space_destroy_ptr(dst);
                return -12;
            }
            dst->high_l2[idx] = (word *)dst_l2_pa;
            dst->high_mask[w] |= 1UL << bit;
            if (!clone_l2_range(src_l2, (word *)dst_l2_pa, 0, 512, &need_sync)) {
                space_destroy_ptr(dst);
                return -12;
            }
        }
    }
    if (need_sync && bank_bound[bank] == src)
        vm_sync();
    return dst_id;
}

static long op_map_anon(unsigned bank, struct guest_space *s, word va, word len, word prot) {
    if (!valid_guest_range(va, len) || (prot & ~7UL)) return -22;
    word pages = len / PAGE;
    int had_valid = 0;
    if (!prot) {
        /* Unmap any existing pages; PROT_NONE needs no page tables. */
        for (word i = 0; i < pages; ++i) {
            word addr = va + i * PAGE;
            word *l3 = ensure_l3(s, bank, addr, 0);
            if (!l3) continue;
            word *pte = &l3[(addr >> 12) & 511];
            if (*pte) {
                if (*pte & PTE_VALID) had_valid = 1;
                release_pte(*pte);
                *pte = 0;
            }
        }
        if (had_valid) flush_range(s, bank, va, pages);
        return 0;
    }
    word defer = (prot_bits(prot) & ~PTE_VALID) | PTE_DEFER;
    for (word i = 0; i < pages; ++i) {
        word addr = va + i * PAGE;
        word *l3 = ensure_l3(s, bank, addr, 1);
        if (!l3) return -12;
        word *pte = &l3[(addr >> 12) & 511];
        if (*pte) {
            if (*pte & PTE_VALID) had_valid = 1;
            release_pte(*pte);
        }
        *pte = defer;
    }
    if (had_valid) flush_range(s, bank, va, pages);
    return 0;
}

static long op_map_file(unsigned bank, struct guest_space *s, word va, word len, word prot,
                        const u8 *data, word file_size, word off, word file_bytes) {
    if (!valid_guest_range(va, len) || (prot & ~7UL) || (off & (PAGE - 1))) return -22;
    if (file_bytes > len || off > file_size) return -22;
    if (file_bytes > file_size - off) file_bytes = file_size - off;
    word pages = len / PAGE;
    word attr = prot ? prot_bits(prot) : 0;
    word defer = prot ? ((attr & ~PTE_VALID) | PTE_DEFER) : 0;
    int had_valid = 0, need_icache = 0;
    for (word i = 0; i < pages; ++i) {
        word pos = i * PAGE, addr = va + pos;
        if (!prot) {
            word *l3 = ensure_l3(s, bank, addr, 0);
            if (l3 && l3[(addr >> 12) & 511]) {
                if (l3[(addr >> 12) & 511] & PTE_VALID) had_valid = 1;
                release_pte(l3[(addr >> 12) & 511]);
                l3[(addr >> 12) & 511] = 0;
            }
            continue;
        }
        word *l3 = ensure_l3(s, bank, addr, 1);
        if (!l3) {
            if (need_icache) guest_icache_sync();
            if (had_valid) flush_range(s, bank, va, pages);
            return -12;
        }
        word *pte = &l3[(addr >> 12) & 511];
        if (*pte) {
            if (*pte & PTE_VALID) had_valid = 1;
            release_pte(*pte);
            *pte = 0;
        }
        if (pos >= file_bytes) {
            *pte = defer;
            continue;
        }
        word pa = alloc_page(0);
        if (!pa) {
            if (need_icache) guest_icache_sync();
            if (had_valid) flush_range(s, bank, va, pages);
            return -12;
        }
        word take = file_bytes - pos;
        if (take > PAGE) take = PAGE;
        memcpy((void *)pa, data + off + pos, take);
        if (take < PAGE) memset((void *)(pa + take), 0, PAGE - take);
        if (prot & 4) {
            guest_clean_dcache(pa, PAGE);
            need_icache = 1;
        }
        *pte = pa | attr | PTE_FILE;
    }
    if (need_icache) guest_icache_sync();
    if (had_valid) flush_range(s, bank, va, pages);
    else __asm__ volatile("dsb ishst\nisb" ::: "memory");
    return 0;
}

static long op_map_shared(unsigned bank, struct guest_space *s, word va, word len, word prot,
                          word shared_va) {
    if (!valid_guest_range(va, len) || (prot & ~7UL) || (shared_va & (PAGE - 1))) return -22;
    if (shared_va < LX_SHARED_BASE || shared_va >= LX_SHARED_BASE + LX_SHARED_APERTURE ||
        len > LX_SHARED_BASE + LX_SHARED_APERTURE - shared_va)
        return -22;
    word pages = len / PAGE;
    int want_write = !!(prot & 2);
    for (word i = 0; i < pages; ++i)
        if (!guest_va_to_pa(shared_va + i * PAGE, want_write)) return -14;
    word attr = prot ? (prot_bits(prot) | PTE_SHARED) : 0;
    int had_valid = 0;
    for (word i = 0; i < pages; ++i) {
        word addr = va + i * PAGE;
        word *l3 = ensure_l3(s, bank, addr, prot != 0);
        if (!l3) {
            if (prot) return -12;
            continue;
        }
        word *pte = &l3[(addr >> 12) & 511];
        if (*pte) {
            if (*pte & PTE_VALID) had_valid = 1;
            release_pte(*pte);
            *pte = 0;
        }
        if (prot) {
            word pa = guest_va_to_pa(shared_va + i * PAGE, want_write);
            *pte = pa | attr;
        }
    }
    if (had_valid) flush_range(s, bank, va, pages);
    else __asm__ volatile("dsb ishst\nisb" ::: "memory");
    return 0;
}

static long op_map_alias(unsigned bank, struct guest_space *dst, word dst_va, word len, word prot,
                         struct guest_space *src, word src_va) {
    if (!valid_guest_range(dst_va, len) || !valid_guest_range(src_va, len) || (prot & ~7UL))
        return -22;
    word pages = len / PAGE;
    word attr = prot ? prot_bits(prot) : 0;
    int had_valid = 0, src_sync = 0, need_icache = 0;
    for (word i = 0; i < pages; ++i) {
        word sva = src_va + i * PAGE, dva = dst_va + i * PAGE;
        word *sl3 = ensure_l3(src, bank, sva, 0);
        word spte = sl3 ? sl3[(sva >> 12) & 511] : 0;
        if (!spte) return -14;
        word *dl3 = ensure_l3(dst, bank, dva, 1);
        if (!dl3) return -12;
        word *dpte = &dl3[(dva >> 12) & 511];
        if (*dpte) {
            if (*dpte & PTE_VALID) had_valid = 1;
            release_pte(*dpte);
            *dpte = 0;
        }
        if (!prot) continue;
        if (spte & PTE_DEFER) {
            *dpte = (attr & ~PTE_VALID) | PTE_DEFER;
        } else if (spte & PTE_SHARED) {
            *dpte = (spte & PA_MASK) | attr | PTE_SHARED;
        } else if (spte & PA_MASK) {
            word pa = spte & PA_MASK;
            page_ref_inc(pa);
            word next = pa | attr | (spte & PTE_FILE) | PTE_COW;
            if (prot & 2) next |= PTE_RO;
            if (!(spte & PTE_RO) || !(spte & PTE_COW)) {
                if (!(spte & PTE_RO) && (spte & PTE_VALID)) src_sync = 1;
                sl3[(sva >> 12) & 511] = spte | PTE_RO | PTE_COW;
            }
            if ((prot & 4) && (!(spte & PTE_VALID) || (spte & PTE_UXN))) {
                guest_clean_dcache(pa, PAGE);
                need_icache = 1;
            }
            *dpte = next;
        }
    }
    if (need_icache) guest_icache_sync();
    if (src_sync && bank_bound[bank] == src) vm_sync();
    if (had_valid) flush_range(dst, bank, dst_va, pages);
    else __asm__ volatile("dsb ishst\nisb" ::: "memory");
    return 0;
}

static long op_protect(unsigned bank, struct guest_space *s, word va, word len, word prot) {
    if (!valid_guest_range(va, len) || (prot & ~7UL)) return -22;
    word pages = len / PAGE;
    word attr = prot ? prot_bits(prot) : 0;
    int had_valid = 0, need_icache = 0;
    for (word i = 0; i < pages; ++i) {
        word addr = va + i * PAGE;
        word *l3 = ensure_l3(s, bank, addr, prot != 0);
        if (!l3) {
            if (prot) {
                if (need_icache) guest_icache_sync();
                if (had_valid) flush_range(s, bank, va, pages);
                return -12;
            }
            continue;
        }
        word *pte = &l3[(addr >> 12) & 511];
        word old = *pte;
        if (!old) {
            if (prot) *pte = (attr & ~PTE_VALID) | PTE_DEFER;
            continue;
        }
        if (!prot) {
            /* Keep pages resident or unmapped? PROT_NONE drops access; clear VALID & DEFER if zero. */
            if (old & PTE_VALID) {
                had_valid = 1;
                *pte = (old & (PA_MASK | PTE_COW | PTE_SHARED | PTE_FILE)) | PTE_UXN | PTE_PXN | PTE_RO;
            } else if (old & PTE_DEFER) {
                *pte = PTE_DEFER | PTE_UXN | PTE_PXN | PTE_RO;
            }
            continue;
        }
        if (old & PTE_DEFER) {
            *pte = (attr & ~PTE_VALID) | PTE_DEFER;
            continue;
        }
        word pa = old & PA_MASK;
        if (!pa) continue;
        if (old & PTE_VALID) had_valid = 1;
        word keep = old & (PTE_SHARED | PTE_FILE);
        word next = pa | attr | keep;
        if (!(old & PTE_SHARED) && ((old & PTE_COW) || page_ref_get(pa) > 1)) {
            next |= PTE_COW;
            if (prot & 2) next |= PTE_RO;
        }
        if ((prot & 4) && (!(old & PTE_VALID) || (old & PTE_UXN))) {
            guest_clean_dcache(pa, PAGE);
            need_icache = 1;
        }
        *pte = next;
    }
    if (need_icache) guest_icache_sync();
    if (had_valid) flush_range(s, bank, va, pages);
    else __asm__ volatile("dsb ishst\nisb" ::: "memory");
    return 0;
}

static long op_unmap(unsigned bank, struct guest_space *s, word va, word len) {
    if (!valid_guest_range(va, len)) return -22;
    word pages = len / PAGE;
    int had_valid = 0;
    for (word i = 0; i < pages; ++i) {
        word addr = va + i * PAGE;
        word *l3 = ensure_l3(s, bank, addr, 0);
        if (!l3) continue;
        word *pte = &l3[(addr >> 12) & 511];
        if (*pte) {
            if (*pte & PTE_VALID) had_valid = 1;
            release_pte(*pte);
            *pte = 0;
        }
    }
    if (had_valid) flush_range(s, bank, va, pages);
    return 0;
}

static long op_discard(unsigned bank, struct guest_space *s, word va, word len) {
    if (!valid_guest_range(va, len)) return -22;
    word pages = len / PAGE;
    int had_valid = 0;
    for (word i = 0; i < pages; ++i) {
        word addr = va + i * PAGE;
        word *l3 = ensure_l3(s, bank, addr, 0);
        if (!l3) continue;
        word *pte = &l3[(addr >> 12) & 511];
        word old = *pte;
        if (!old) continue;
        if (old & (PTE_FILE | PTE_SHARED)) return -95;
        if (!(old & PTE_DEFER) && (old & PA_MASK)) {
            if (old & PTE_VALID) had_valid = 1;
            page_ref_dec(old & PA_MASK);
            word perms = old & ~(PA_MASK | PTE_VALID | PTE_COW);
            if (old & PTE_COW) perms &= ~PTE_RO;
            *pte = perms | PTE_DEFER;
        }
    }
    if (had_valid) flush_range(s, bank, va, pages);
    return 0;
}

int guest_handle_fault(word esr, word far) {
    unsigned ec = esr >> 26;
    if (ec != 0x20 && ec != 0x24) return 0;
    if (!((far >= LX_GUEST_LOW_BASE && far < LX_GUEST_LOW_END) ||
          (far >= LX_GUEST_HIGH_BASE && far < LX_GUEST_HIGH_END)))
        return 0;
    unsigned bank = kt_owner(kt_slot());
    struct guest_space *s = bank < MAX_BANKS ? bank_bound[bank] : 0;
    if (!s) return 0;
    word va = far & ~(PAGE - 1);
    word *l3 = ensure_l3(s, bank, va, 0);
    if (!l3) return 0;
    word *pte = &l3[(va >> 12) & 511];
    word entry = *pte;
    if (!entry) return 0;
    int write = (ec == 0x24) && !!(esr & 64);
    int exec = (ec == 0x20);
    unsigned dfsc = esr & 0x3f;
    /* Demand-zero page fault (translation fault level 3 = 7, or level 1/2/3 in 4..7). */
    if ((dfsc & 0x3c) == 4 && (entry & PTE_DEFER)) {
        if (!(entry & PTE_EL0) || (write && (entry & PTE_RO)) || (exec && (entry & PTE_UXN)))
            return 0;
        word pa = alloc_page(1);
        if (!pa) return 0;
        if (!(entry & PTE_UXN)) {
            guest_clean_dcache(pa, PAGE);
            guest_icache_sync();
        }
        *pte = pa | (entry & ~(PA_MASK | PTE_DEFER)) | PTE_VALID;
        __asm__ volatile("dsb ishst\nisb" ::: "memory");
        return 1;
    }
    /* Copy-on-Write permission fault (permission fault = 0x0c..0x0f). */
    if ((dfsc & 0x3c) == 0x0c && write && (entry & PTE_VALID) && (entry & PTE_COW)) {
        word old_pa = entry & PA_MASK;
        if (page_ref_get(old_pa) <= 1) {
            *pte = entry & ~(PTE_RO | PTE_COW);
            guest_tlbi_va(va);
            return 1;
        }
        word new_pa = alloc_page(0);
        if (!new_pa) return 0;
        memcpy((void *)new_pa, (void *)old_pa, PAGE);
        if (!(entry & PTE_UXN)) {
            guest_clean_dcache(new_pa, PAGE);
            guest_icache_sync();
        }
        page_ref_dec(old_pa);
        *pte = new_pa | ((entry & ~(PA_MASK | PTE_RO | PTE_COW)) | PTE_VALID);
        guest_tlbi_va(va);
        return 1;
    }
    return 0;
}

/* Hooks called by guest-arch.S and the task scheduler. */
void guest_record_enter(word saved_sp) {
    unsigned slot = kt_slot();
    unsigned cpu = smp_cpu();
    gtasks[slot].saved_sp = saved_sp;
    gtasks[slot].active = 1;
    guest_running[cpu] = &gtasks[slot].shadow;
    smp_revoke_clock(cpu);
}

word guest_finish_exit(void) {
    unsigned slot = kt_slot();
    unsigned cpu = smp_cpu();
    gtasks[slot].active = 0;
    guest_running[cpu] = 0;
    return gtasks[slot].saved_sp;
}

void guest_schedule_switch(unsigned cpu, unsigned slot) {
    guest_running[cpu] = (slot < KT_TASK_SLOTS && gtasks[slot].active)
                             ? &gtasks[slot].shadow
                             : 0;
}

/* Called from smp_irq_vector with pointer to the 816-byte IRQ frame on SP_EL1.
 * Returns saved_sp to unwind back to host, or 0 to resume guest EL0. */
word guest_irq_check(void *frame) {
    unsigned slot = kt_slot();
    if (slot >= KT_TASK_SLOTS || !gtasks[slot].active) return 0;
    int stop = smp_check_stop();
    int expired = gtasks[slot].deadline && smp_ticks() >= gtasks[slot].deadline;
    if (!stop && !expired) return 0;
    const word *f = (const word *)frame;
    struct lx_guest_ctx *g = &gtasks[slot].shadow;
    g->pc = f[0];
    g->pstate = f[1];
    g->sp = f[2];
    g->tpidr = f[3];
    g->fpcr = f[4];
    g->fpsr = f[5];
    for (unsigned i = 0; i < 16; ++i)
        memcpy(&g->v[i * 4], &f[6 + (15 - i) * 4], 32);
    g->x[30] = f[70];
    for (unsigned i = 0; i < 15; ++i) {
        g->x[2 * i]     = f[72 + (14 - i) * 2];
        g->x[2 * i + 1] = f[72 + (14 - i) * 2 + 1];
    }
    g->esr = 0;
    g->far = 0;
    g->exit_reason = stop ? LX_GUEST_EXIT_STOP : LX_GUEST_EXIT_PREEMPT;
    gtasks[slot].active = 0;
    guest_running[smp_cpu()] = 0;
    return gtasks[slot].saved_sp;
}

static int user_accessible(word addr, word bytes, int write) {
    if (!addr || !bytes || addr + bytes < addr) return 0;
    word first = addr & ~(PAGE - 1);
    word last = (addr + bytes - 1) & ~(PAGE - 1);
    for (word p = first; ; p += PAGE) {
        if (!kt_access(p, write) && !(vm_touch(p, write, 0) && kt_access(p, write)))
            return 0;
        if (p == last) return 1;
    }
}

int guest_read_req(word addr, word out[8]) {
    if ((addr & 7) || !user_accessible(addr, 8 * sizeof(word), 0)) return 0;
    memcpy(out, (const void *)addr, 8 * sizeof(word));
    return 1;
}

long guest_call(unsigned bank, const word *req, const u8 *file_data, word file_size) {
    word op = req[0];
    if (op == LX_GUEST_QUERY) return LX_GUEST_ABI_VER;
    if (op == LX_GUEST_SPACE_CREATE) return space_create(bank);
    struct guest_space *s = get_space(bank, req[1]);
    if (!s) return -22;
    switch (op) {
    case LX_GUEST_SPACE_DESTROY:
        space_destroy_ptr(s);
        return 0;
    case LX_GUEST_SPACE_CLONE:
        return space_clone(bank, s);
    case LX_GUEST_MAP_ANON:
        return op_map_anon(bank, s, req[2], req[3], req[4]);
    case LX_GUEST_MAP_FILE:
        if (req[7] && !file_data) return -9;
        return op_map_file(bank, s, req[2], req[3], req[4], file_data, file_size, req[6], req[7]);
    case LX_GUEST_MAP_SHARED:
        return op_map_shared(bank, s, req[2], req[3], req[4], req[5]);
    case LX_GUEST_MAP_ALIAS: {
        struct guest_space *src = get_space(bank, req[5]);
        if (!src) return -22;
        return op_map_alias(bank, s, req[2], req[3], req[4], src, req[6]);
    }
    case LX_GUEST_PROTECT:
        return op_protect(bank, s, req[2], req[3], req[4]);
    case LX_GUEST_UNMAP:
        return op_unmap(bank, s, req[2], req[3]);
    case LX_GUEST_DISCARD:
        return op_discard(bank, s, req[2], req[3]);
    case LX_GUEST_BIND:
        bind_space(bank, s);
        return 0;
    case LX_GUEST_RUN: {
        word ctx_va = req[2], slice = req[3];
        if ((ctx_va & 15) || !user_accessible(ctx_va, sizeof(struct lx_guest_ctx), 1))
            return -14;
        bind_space(bank, s);
        unsigned slot = kt_slot();
        struct guest_task *gt = &gtasks[slot];
        memcpy(&gt->shadow, (const void *)ctx_va, sizeof(struct lx_guest_ctx));
        gt->deadline = slice ? smp_ticks() + slice : 0;
        kt_service_leave();
        long reason = guest_enter(&gt->shadow);
        kt_service_enter();
        memcpy((void *)ctx_va, &gt->shadow, sizeof(struct lx_guest_ctx));
        return reason;
    }
    default:
        return -38;
    }
}

void guest_cleanup_bank(unsigned bank) {
    if (bank >= MAX_BANKS) return;
    if (bank_bound[bank]) bind_space(bank, 0);
    for (unsigned i = 0; i < MAX_SPACES; ++i)
        if (spaces[i].used && spaces[i].owner_bank == bank)
            space_destroy_ptr(&spaces[i]);
}
