/* SVC 60. The process-private launch mailbox carries one packet per call.
 * The kernel owns context IDs, resource names and all device-visible memory. */
#ifndef LIXIN_GPU_ABI_H
#define LIXIN_GPU_ABI_H
enum lx_gpu_operation {
    LX_GPU_OPEN, LX_GPU_CLOSE, LX_GPU_CAPS, LX_GPU_CREATE, LX_GPU_DROP,
    LX_GPU_WRITE, LX_GPU_READ, LX_GPU_UPLOAD, LX_GPU_DOWNLOAD,
    LX_GPU_COMMAND_PART, LX_GPU_SUBMIT
};
enum { LX_GPU_MAIL_SIZE = 32768, LX_GPU_COMMAND_MAX = 65 * 4096 };
struct lx_gpu_create {
    unsigned target, format, bind, width, height, depth, layers, last_level, samples, flags, bytes;
};
struct lx_gpu_transfer {
    unsigned id, level, stride, layer_stride, offset, x, y, z, width, height, depth;
};
struct lx_gpu_chunk { unsigned id, offset, bytes; unsigned char data[]; };
#endif
