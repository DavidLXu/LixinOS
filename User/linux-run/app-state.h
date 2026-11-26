// Choose writeback storage for new app profiles on LixinOS build volumes.
#pragma once
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static inline char **app_state_dirs(char **legacy, char **writeback) {
    struct stat st;
    if (stat("/scratch", &st) || !S_ISDIR(st.st_mode))
        return legacy;
    DIR *dir = opendir(legacy[0]);
    if (dir) {
        // Old templates may contain an empty placeholder. Preserve any
        // existing profile, even one with files written by another program.
        struct dirent *entry;
        errno = 0;
        while ((entry = readdir(dir))) {
            if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
                break;
        }
        int occupied = entry != NULL || errno != 0;
        closedir(dir);
        if (occupied)
            return legacy;
    } else if (errno != ENOENT) {
        return legacy;
    }
    if ((mkdir(writeback[0], 0700) && errno != EEXIST) ||
        stat(writeback[0], &st) || !S_ISDIR(st.st_mode))
        return legacy;
    return writeback;
}

static inline int app_state_sync(const char *root) {
    int fd = open(root, O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        return -1;
    if (fsync(fd) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return close(fd);
}
