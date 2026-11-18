#pragma once
typedef struct lx_dir DIR;
struct dirent {
    char d_name[513];
};
DIR *opendir(const char *);
struct dirent *readdir(DIR *);
int closedir(DIR *);
