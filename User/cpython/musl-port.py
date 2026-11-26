"""Route musl's AArch64 system calls to the LixinOS userspace adapter."""
from pathlib import Path
import sys

root = Path(sys.argv[1])
header = """#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)
#define IPC_64 0
long lx_posix(long, long, long, long, long, long, long);
"""
for n in range(7):
    args = list("abcdef"[:n])
    params = ", ".join("long " + a for a in ["n", *args])
    values = ", ".join(["n", *args, *(["0"] * (6-n))])
    header += f"static inline long __syscall{n}({params}) {{ return lx_posix({values}); }}\n"
(root / "arch/aarch64/syscall_arch.h").write_text(header)
# The default cancellable syscall path, clone, and vfork contain raw Linux SVCs.
(root / "src/thread/aarch64/syscall_cp.s").write_text("""
.global __cp_begin, __cp_end, __cp_cancel, __syscall_cp_asm
.hidden __cp_begin, __cp_end, __cp_cancel, __syscall_cp_asm
__syscall_cp_asm:
__cp_begin:
 mov x0,x1
 mov x1,x2
 mov x2,x3
 mov x3,x4
 mov x4,x5
 mov x5,x6
 mov x6,x7
 b lx_posix
__cp_end:
__cp_cancel:
 b __cancel
""")
(root / "src/thread/aarch64/clone.s").write_text(".global __clone\n.hidden __clone\n__clone:\n b lx_clone\n")
(root / "src/process/aarch64/vfork.s").write_text("""
.global vfork
vfork:
 mov x0,#-38
 b __syscall_ret
""")
# musl normally finds the main stack via Linux mremap probes. The launcher
# knows its bounds and keeps the low end separate from static interpreter data.
init = root / "src/env/__init_tls.c"
text = init.read_text()
if "lx_main_stack" not in text:
    init.write_text(text.replace("td->self = td;", """td->self = td;
    extern unsigned char __end_image[] __attribute__((__weak__));
    if (__end_image) {
        size_t lx_main_stack = 2UL << 20;
        td->stack_size = lx_main_stack;
        td->stack = __end_image + lx_main_stack;
    }"""))

text = init.read_text()
if "lx_stack_top" not in text:
    text = text.replace("int r = __set_thread_area", """/* The dynamic ELF bootstrap supplies the main stack top in auxv. */
    if (!__end_image && libc.auxv) {
        for (size_t *aux = libc.auxv; aux[0]; aux += 2) {
            if (aux[0] == 0x6c780001UL) {
                size_t lx_stack_top = aux[1];
                td->stack = (void *)lx_stack_top;
                td->stack_size = 2UL << 20;
                break;
            }
        }
    }
    int r = __set_thread_area""")
    init.write_text(text)

(root / "src/thread/aarch64/__unmapself.s").write_text(".global __unmapself\n.hidden __unmapself\n__unmapself:\n b lx_thread_exit\n")
# The guest allocates stacks in the same paged arena as interpreter memory.
p = root / "src/thread/pthread_create.c"
s = p.read_text()
s = s.replace("guard = ROUND(attr._a_guardsize);", "guard = 0; /* LixinOS user arena */")
p.write_text(s)
p = root / "src/network/lookup_name.c"
s = p.read_text().replace("if (!cnt) cnt = name_from_dns_search(buf, canon, name, family);",
"""if (!cnt) {
            extern int lx_lookup_ipv4(const char *, unsigned char *);
            if (family == AF_INET6 || lx_lookup_ipv4(name, buf[0].addr) < 0) return EAI_NONAME;
            buf[0].family = AF_INET;
            cnt = 1;
        }""")
p.write_text(s)
