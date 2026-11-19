// Target defaults for C applications using the LixinOS static musl ABI.
#undef TARGET_OS_CPP_BUILTINS
#define TARGET_OS_CPP_BUILTINS() do { builtin_define("__lixinos__"); builtin_define("__unix__"); } while (0)
#undef CPP_SPEC
#define CPP_SPEC "%{!nostdinc:-isystem /lib/gcc/include}"
#undef STARTFILE_SPEC
#define STARTFILE_SPEC "%{!r:/lib/gcc/crt0.o /lib/gcc/crt1.o}"
#undef ENDFILE_SPEC
#define ENDFILE_SPEC ""
#undef LIB_SPEC
#define LIB_SPEC "--start-group -llixin -lc -lgcc --end-group"
#undef LINK_SPEC
#define LINK_SPEC "%{shared:%eLixinOS supports static executables only} " \
                  "%{pie:%eLixinOS requires fixed-address executables} " \
                  "-L/lib/gcc %{!r:-static --gc-sections -z max-page-size=4096 %{!T*:-T /lib/gcc/application.ld}}"
