#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

/* Read-only document and font data. Some guest kernels only map anonymous memory. */
void *lx_writer_map_readonly(int fd, size_t length, off_t offset)
{
    void *data = mmap(NULL, length, PROT_READ, MAP_PRIVATE, fd, offset);
    if (data != MAP_FAILED || !length)
        return data;
    data = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (data == MAP_FAILED)
        return data;
    size_t done = 0;
    while (done < length) {
        ssize_t n = pread(fd, (char *)data + done, length - done, offset + done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            int error = n < 0 ? errno : EIO;
            munmap(data, length);
            errno = error;
            return MAP_FAILED;
        }
        done += n;
    }
    return data;
}
