#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { printf("VM failure at %d, errno %d\n", __LINE__, errno); return 1; } } while (0)

int main(int argc, char **argv) {
    size_t page = sysconf(_SC_PAGESIZE);
    char *p = mmap(NULL, 3 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    CHECK(!mprotect(p + page, page, PROT_READ | PROT_WRITE));
    CHECK(!p[page]);
    p[page] = 42;
    CHECK(!mprotect(p + page, page, PROT_NONE));
    CHECK(!mprotect(p + page, page, PROT_READ));
    CHECK(p[page] == 42);
    if (argc == 2 && !strcmp(argv[1], "readonly")) {
        ((volatile char *)p)[page] = 2;
        puts("unexpected write");
        return 1;
    }
    CHECK(!madvise(p + page,page,MADV_DONTNEED));
    CHECK(!p[page]);
    CHECK(!munmap(p + page, page));
    CHECK(mprotect(p, 2 * page, PROT_READ) == -1 && errno == ENOMEM);
    if (argc == 2 && !strcmp(argv[1], "unmapped")) {
        printf("unexpected read %d\n", ((volatile char *)p)[page]);
        return 1;
    }
    CHECK(mmap(p, page, PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED_NOREPLACE, -1, 0)
          == MAP_FAILED && errno == EEXIST);
    CHECK(mmap(p + page, page, PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED_NOREPLACE, -1, 0)
          == p + page && p[page] == 0);
    CHECK(!munmap(p, 3 * page));

    int fd = open("/vm-input", O_RDONLY);
    CHECK(fd >= 0 && lseek(fd, 11, SEEK_SET) == 11);
    char *first = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, page);
    CHECK(first != MAP_FAILED && lseek(fd, 0, SEEK_CUR) == 11);
    char *second = mmap(NULL, page, PROT_READ, MAP_PRIVATE, fd, page);
    CHECK(second != MAP_FAILED && !close(fd));
    CHECK(!memcmp(first, "private", 7) && !memcmp(second, first, 7) && first[7] == 0);
    first[0] = 'X';
    CHECK(second[0] == 'p');
    CHECK(madvise(first,page,MADV_DONTNEED) == -1 && errno == ENOTSUP && first[0]=='X');
    if (argc == 2 && !strcmp(argv[1], "past-eof")) {
        printf("unexpected byte %d\n", ((volatile char *)first)[page]);
        return 1;
    }
    CHECK(!munmap(first, 2 * page) && !munmap(second, page));
    fd = open("/vm-input", O_RDONLY);
    char tail[7];
    CHECK(fd >= 0 && pread(fd, tail, 7, page) == 7 && !memcmp(tail, "private", 7));
    CHECK(!close(fd));
    puts("VM mappings OK");
    return 0;
}
