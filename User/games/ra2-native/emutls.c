// The bare-metal AArch64 compiler emits GCC emulated TLS descriptors. Its
// single-thread runtime shares their addresses, including the Win32 machine
// lock depth and saved x86 registers. Keep an instance per musl pthread.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct tls_descriptor {
    size_t size, alignment;
    void *runtime_slot;
    const void *initial_value;
};

struct tls_instance {
    const struct tls_descriptor *descriptor;
    void *value;
    struct tls_instance *next;
};

static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_key_t instances;

static void release_instances(void *head) {
    struct tls_instance *item = head;
    while (item) {
        struct tls_instance *next = item->next;
        free(item->value);
        free(item);
        item = next;
    }
}

static void create_key(void) {
    if (pthread_key_create(&instances, release_instances)) abort();
}

void *__wrap___emutls_get_address(const struct tls_descriptor *descriptor) {
    pthread_once(&once, create_key);
    struct tls_instance *head = pthread_getspecific(instances);
    for (struct tls_instance *item = head; item; item = item->next)
        if (item->descriptor == descriptor) return item->value;

    struct tls_instance *item = malloc(sizeof(*item));
    size_t alignment = descriptor->alignment;
    if (alignment < sizeof(void *)) alignment = sizeof(void *);
    if (!item || posix_memalign(&item->value, alignment, descriptor->size)) abort();
    if (descriptor->initial_value)
        memcpy(item->value, descriptor->initial_value, descriptor->size);
    else
        memset(item->value, 0, descriptor->size);
    item->descriptor = descriptor;
    item->next = head;
    if (pthread_setspecific(instances, item)) abort();
    return item->value;
}
