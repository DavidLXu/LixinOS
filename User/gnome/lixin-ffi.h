#ifndef LIXINOS_FFI_CLOSURES_H
#define LIXINOS_FFI_CLOSURES_H

#define LX_FFI_SLOTS 4096
#define LX_FFI_TRAMPOLINE_SIZE 32

#ifndef __ASSEMBLER__
int lx_ffi_attach(ffi_closure *closure, void *code, void (*entry)(void));
#endif

#endif
