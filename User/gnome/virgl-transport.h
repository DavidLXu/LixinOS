/* Mesa VirGL resource and command transport for LixinOS. All calls complete
 * on a device fence before the mailbox or resource storage can be reused. */
#include "gpu-abi.h"
extern long lx_console_call(long operation, long value);

static struct lx_gpu_chunk *const gpu_mail = (void *)0x443d8000UL;

static int gpu_chunks(unsigned op, unsigned id, unsigned offset, void *data, unsigned bytes)
{
   unsigned char *p = data;
   while (bytes) {
      unsigned n = MIN2(bytes, LX_GPU_MAIL_SIZE - sizeof(*gpu_mail));
      gpu_mail->id = id;
      gpu_mail->offset = offset;
      gpu_mail->bytes = n;
      if (op != LX_GPU_READ)
         memcpy(gpu_mail->data, p, n);
      if (lx_console_call(60, op) < 0)
         return -1;
      if (op == LX_GPU_READ)
         memcpy(p, gpu_mail->data, n);
      p += n;
      bytes -= n;
      offset += n;
   }
   return 0;
}
static int gpu_transfer(struct virgl_winsys *ws, struct virgl_hw_res *res,
                        const struct pipe_box *box, unsigned stride, unsigned layer,
                        unsigned offset, unsigned level, bool upload)
{
   unsigned row = util_format_get_stride(res->format, box->width);
   unsigned rows = util_format_get_nblocksy(res->format, box->height);
   unsigned pitch = stride ? stride : row;
   uint64_t slice = layer ? layer : (uint64_t)pitch * rows;
   uint64_t bytes = (box->depth-1) * slice + (uint64_t)(rows-1) * pitch + row;
   if (!rows || box->depth < 1 || offset > (unsigned)res->size || bytes > (unsigned)res->size-offset)
      return -1;
   unsigned char *map = virgl_vtest_resource_map(ws, res);
   if (!map) return -1;
   int rc = upload ? gpu_chunks(LX_GPU_WRITE, res->res_handle, offset, map+offset, bytes) : 0;
   if (!rc) {
      struct lx_gpu_transfer transfer = {
         res->res_handle, level, stride, layer, offset, box->x, box->y, box->z,
         box->width, box->height, box->depth
      };
      memcpy(gpu_mail, &transfer, sizeof(transfer));
      rc = lx_console_call(60, upload ? LX_GPU_UPLOAD : LX_GPU_DOWNLOAD);
      if (!rc && !upload)
         rc = gpu_chunks(LX_GPU_READ, res->res_handle, offset, map+offset, bytes);
   }
   virgl_vtest_resource_unmap(ws, res);
   return rc;
}
static int gpu_submit(struct virgl_vtest_cmd_buf *cbuf)
{
   unsigned n = cbuf->base.cdw * 4;
   if (gpu_chunks(LX_GPU_COMMAND_PART, 0, 0, cbuf->base.buf, n) < 0) return -1;
   memcpy(gpu_mail, &n, sizeof(n));
   return lx_console_call(60, LX_GPU_SUBMIT);
}
