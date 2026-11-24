/* A closure uses an executable trampoline from the ELF image and writable
   parameters in ordinary memory. The heap is never executable. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lixin-ffi.h"

struct lx_ffi_slot {
    _Atomic(void (*)(void)) entry;
    ffi_closure *closure;
};

_Static_assert(sizeof(struct lx_ffi_slot) == 16, "trampoline slot layout");
_Static_assert(FFI_TRAMPOLINE_SIZE >= sizeof(unsigned), "slot index storage");
struct lx_ffi_slot lx_ffi_slots[LX_FFI_SLOTS];
extern unsigned char lx_ffi_trampoline_table[];
static pthread_mutex_t slots_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned next_slot;

static void *trampoline(unsigned index) {
    return lx_ffi_trampoline_table + index * LX_FFI_TRAMPOLINE_SIZE;
}

static struct lx_ffi_slot *slot_for(ffi_closure *closure) {
    unsigned index;
    memcpy(&index, closure->tramp, sizeof(index));
    if (index >= LX_FFI_SLOTS || lx_ffi_slots[index].closure != closure)
        return NULL;
    return &lx_ffi_slots[index];
}

void *ffi_closure_alloc(size_t size, void **code) {
    if (!code || size < sizeof(ffi_closure))
        return NULL;
    *code = NULL;
    ffi_closure *closure = malloc(size);
    if (!closure)
        return NULL;

    pthread_mutex_lock(&slots_lock);
    for (unsigned checked = 0; checked < LX_FFI_SLOTS; ++checked) {
        unsigned index = next_slot;
        next_slot = (next_slot + 1) % LX_FFI_SLOTS;
        if (!lx_ffi_slots[index].closure) {
            memcpy(closure->tramp, &index, sizeof(index));
            lx_ffi_slots[index].closure = closure;
            *code = trampoline(index);
            break;
        }
    }
    pthread_mutex_unlock(&slots_lock);
    if (*code)
        return closure;
    free(closure);
    return NULL;
}

int lx_ffi_attach(ffi_closure *closure, void *code, void (*entry)(void)) {
    pthread_mutex_lock(&slots_lock);
    struct lx_ffi_slot *slot = slot_for(closure);
    int valid = slot && code == trampoline(slot - lx_ffi_slots);
    if (valid)
        atomic_store_explicit(&slot->entry, entry, memory_order_release);
    pthread_mutex_unlock(&slots_lock);
    return valid;
}

void ffi_closure_free(void *data) {
    if (!data)
        return;
    pthread_mutex_lock(&slots_lock);
    struct lx_ffi_slot *slot = slot_for(data);
    if (slot) {
        atomic_store_explicit(&slot->entry, NULL, memory_order_release);
        slot->closure = NULL;
    }
    pthread_mutex_unlock(&slots_lock);
    if (slot)
        free(data);
}

void *ffi_data_to_code_pointer(void *data) {
    if (!data)
        return NULL;
    pthread_mutex_lock(&slots_lock);
    struct lx_ffi_slot *slot = slot_for(data);
    void *code = slot ? trampoline(slot - lx_ffi_slots) : data;
    pthread_mutex_unlock(&slots_lock);
    return code;
}
