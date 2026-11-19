/* Physical addresses in the 24 GiB QEMU virt build VM. RAM starts at 1 GiB.
 * Two packing arenas hold the working filesystem and its next checkpoint.
 * Indices and I/O buffers are EL1-only. Process frames back private EL0 maps. */
#define GIB (1UL << 30)
#define BUILD_RAM (24 * GIB)
#define BUILD_FIRST (4 * GIB)
#define BUILD_SECOND (10 * GIB)
#define BUILD_ARENA_BYTES (6 * GIB)
#define BUILD_INDICES (16 * GIB)
#define BUILD_BUFFERS (17 * GIB)
#define BUILD_FRAMES (18 * GIB)
