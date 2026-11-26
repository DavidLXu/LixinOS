// aarch64-none-elf libgcc shares one TLS allocation between all threads.
// Qt's event and GUI threads need separate objects in the guest musl runtime.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct tls_control {
    size_t size, alignment;
    void *reserved;
    const void *initial;
};

struct tls_entry {
    const struct tls_control *control;
    void *value;
    struct tls_entry *next;
};

struct tls_storage {
    struct tls_entry *first;
    int exiting;
};

static pthread_key_t storage_key;
static pthread_once_t key_once = PTHREAD_ONCE_INIT;

static void destroy_storage(void *ptr)
{
    struct tls_storage *storage = ptr;
    // Other pthread key destructors, including Qt's, can still access TLS.
    if (!storage->exiting) {
        storage->exiting = 1;
        if (pthread_setspecific(storage_key, storage))
            abort();
        return;
    }
    while (storage->first) {
        struct tls_entry *entry = storage->first;
        storage->first = entry->next;
        free(entry->value);
        free(entry);
    }
    free(storage);
}

static void create_key(void)
{
    if (pthread_key_create(&storage_key, destroy_storage))
        abort();
}

#ifdef LIBSTDCXX_ONCE_TLS
struct tls_control once_call __asm__("__emutls_v._ZSt11__once_call");
struct tls_control once_callable __asm__("__emutls_v._ZSt15__once_callable");
extern void *lx_once_call(void);
extern void *lx_once_callable(void);
#endif

void *__wrap___emutls_get_address(const struct tls_control *control)
{
#ifdef LIBSTDCXX_ONCE_TLS
    if (control == &once_call)
        return lx_once_call();
    if (control == &once_callable)
        return lx_once_callable();
#endif
    pthread_once(&key_once, create_key);
    struct tls_storage *storage = pthread_getspecific(storage_key);
    if (!storage) {
        storage = calloc(1, sizeof(*storage));
        if (!storage || pthread_setspecific(storage_key, storage))
            abort();
    }
    for (struct tls_entry *entry = storage->first; entry; entry = entry->next)
        if (entry->control == control)
            return entry->value;

    struct tls_entry *entry = malloc(sizeof(*entry));
    size_t alignment = control->alignment < sizeof(void *) ? sizeof(void *) : control->alignment;
    if (!entry || posix_memalign(&entry->value, alignment, control->size))
        abort();
    if (control->initial)
        memcpy(entry->value, control->initial, control->size);
    else
        memset(entry->value, 0, control->size);
    entry->control = control;
    entry->next = storage->first;
    storage->first = entry;
    return entry->value;
}
