#ifndef LX_VM_H
#define LX_VM_H

/* SVC 62 uses the utility mailbox, six unsigned 64-bit words:
 * address, length, POSIX protections, fixed-address flags, raw file descriptor, offset. */
#define LX_VM_BASE (96UL << 30)
#define LX_VM_DIRS 16
#define LX_VM_SIZE ((unsigned long)LX_VM_DIRS << 30)
enum { LX_VM_QUERY, LX_VM_MAP, LX_VM_PROTECT, LX_VM_UNMAP, LX_VM_FILE, LX_VM_DISCARD };
struct vm_space {
    unsigned long *dirs[LX_VM_DIRS];
    unsigned long hint;
};
long vm_call(struct vm_space *, unsigned bank, unsigned long op, const unsigned long *args,
             const unsigned char *file, unsigned long size);
void vm_destroy(struct vm_space *);
int vm_touch(unsigned long address, int write, int execute);
int vm_readable(const struct vm_space *, unsigned long address, unsigned long length);
void *vm_allocate(unsigned long size);
void vm_release(void *memory, unsigned long size);
void vm_publish(unsigned bank, const struct vm_space *space);
void vm_sync(void);
void vm_sync_code(unsigned long physical);
int vm_memory_low(void);
unsigned vm_swap_total(void);
unsigned vm_swap_free(void);
unsigned vm_swap_limit(void);
unsigned vm_swap_save(void *frame);
int vm_swap_load(unsigned id, void *frame);
void vm_swap_discard(unsigned id);
#endif
