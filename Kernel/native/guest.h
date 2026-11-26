#ifndef LX_GUEST_H
#define LX_GUEST_H

/* Native AArch64 EL0 guest execution and sparse Copy-on-Write address spaces.
 * SVC 66 accepts a pointer to an eight-word request array in EL0 memory. */
#define LX_GUEST_SVC_NR    66
#define LX_GUEST_ABI_VER   1

/* Guest virtual address windows, disjoint from kernel MMIO, kernel RAM, and
 * the enclosing LixinOS process (1 GiB .. 112 GiB). */
#define LX_GUEST_LOW_BASE  0x0c000000UL        /* 192 MiB */
#define LX_GUEST_LOW_END   0x40000000UL        /* 1024 MiB (832 MiB low < 4 GiB) */
#define LX_GUEST_HIGH_BASE (128UL << 30)       /* 128 GiB */
#define LX_GUEST_HIGH_END  (512UL << 30)       /* 512 GiB (384 GiB high) */

enum {
    LX_GUEST_QUERY         = 0,  /* -> ABI version (1) */
    LX_GUEST_SPACE_CREATE  = 1,  /* -> space_id >= 1 */
    LX_GUEST_SPACE_DESTROY = 2,  /* [1]=space_id */
    LX_GUEST_SPACE_CLONE   = 3,  /* [1]=src_space_id -> dst_space_id (CoW) */
    LX_GUEST_MAP_ANON      = 4,  /* [1]=space, [2]=va, [3]=len, [4]=prot */
    LX_GUEST_MAP_FILE      = 5,  /* [1]=space, [2]=va, [3]=len, [4]=prot, [5]=fd, [6]=off, [7]=file_bytes */
    LX_GUEST_MAP_SHARED    = 6,  /* [1]=space, [2]=va, [3]=len, [4]=prot, [5]=shared_va */
    LX_GUEST_MAP_ALIAS     = 7,  /* [1]=dst_space, [2]=dst_va, [3]=len, [4]=prot, [5]=src_space, [6]=src_va */
    LX_GUEST_PROTECT       = 8,  /* [1]=space, [2]=va, [3]=len, [4]=prot */
    LX_GUEST_UNMAP         = 9,  /* [1]=space, [2]=va, [3]=len */
    LX_GUEST_DISCARD       = 10, /* [1]=space, [2]=va, [3]=len (MADV_DONTNEED) */
    LX_GUEST_BIND          = 11, /* [1]=space */
    LX_GUEST_RUN           = 12, /* [1]=space, [2]=ctx_ptr, [3]=slice_ticks -> exit_reason */
};

enum {
    LX_GUEST_EXIT_SVC     = 1,
    LX_GUEST_EXIT_FAULT   = 2,
    LX_GUEST_EXIT_SYNC    = 3,
    LX_GUEST_EXIT_PREEMPT = 4,
    LX_GUEST_EXIT_STOP    = 5,
};

/* Keep offsets identical to guest-arch.S (832 bytes, 16-byte aligned). */
struct lx_guest_ctx {
    unsigned long x[31];       /* +0   .. +247 */
    unsigned long sp;          /* +248 */
    unsigned long pc;          /* +256 */
    unsigned long pstate;      /* +264 */
    unsigned long tpidr;       /* +272 */
    unsigned long fpcr;        /* +280 */
    unsigned long fpsr;        /* +288 */
    unsigned long exit_reason; /* +296 */
    unsigned long esr;         /* +304 */
    unsigned long far;         /* +312 */
    unsigned long v[64];       /* +320 .. +831 (q0..q31) */
};

#ifndef LX_USER_GUEST_ONLY
int guest_read_req(unsigned long addr, unsigned long out[8]);
long guest_call(unsigned bank, const unsigned long *req,
                const unsigned char *file_data, unsigned long file_size);
int guest_handle_fault(unsigned long esr, unsigned long far);
void guest_cleanup_bank(unsigned bank);
unsigned long guest_get_bound(unsigned bank);
void guest_set_bound(unsigned bank, unsigned long space_id);
void guest_rebind_cpu(unsigned cpu, unsigned bank);
void guest_schedule_switch(unsigned cpu, unsigned slot);
void guest_publish_l1(unsigned bank, unsigned slot, unsigned long entry);
void guest_publish_cpu_l1(unsigned cpu, unsigned slot, unsigned long entry);
#endif

#endif
