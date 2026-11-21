#ifndef LX_FILE_PATH_H
#define LX_FILE_PATH_H
#include <string.h>

/* Resolve a typed location against an absolute directory, including dot and parent components. */
static inline int ui_file_path(char out[513],const char *directory,const char *input) {
    if (!*input) return 0;
    strcpy(out,*input=='/' ? "/" : directory);
    while (*input) {
        if (*input=='/') { ++input; continue; }
        const char *end=input;
        while (*end && *end!='/') ++end;
        int n=end-input;
        if (n==2 && !memcmp(input,"..",2)) {
            char *last=strrchr(out,'/');
            last[last==out]=0;
        } else if (n!=1 || *input!='.') {
            int used=strlen(out);
            if (used+(used>1)+n>512) return 0;
            if (used>1) out[used++]='/';
            memcpy(out+used,input,n);
            out[used+n]=0;
        }
        input=end;
    }
    return 1;
}
#endif
