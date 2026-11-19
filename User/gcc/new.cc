// GCC is built with exceptions disabled. Use the same allocator as its C code.
#include <stddef.h>
#include <stdlib.h>

void *operator new(size_t bytes) {
    void *memory = malloc(bytes ? bytes : 1);
    if (!memory) abort();
    return memory;
}
void *operator new[](size_t bytes) { return ::operator new(bytes); }
void operator delete(void *memory) throw() { free(memory); }
void operator delete[](void *memory) throw() { free(memory); }
