#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(expr) do { if (!(expr)) {printf("identity failure line %d errno %d\n",__LINE__,errno);return 1;} } while (0)
int main(int argc,char **argv) {
    const char *path="/tmp/posix-owned",*secret="/tmp/posix-private";
    struct stat st;
    CHECK(geteuid()==0);
    if (argc==2 && !strcmp(argv[1],"verify")) {
        CHECK(!stat(path,&st) && st.st_uid==1101 && st.st_gid==1102 && (st.st_mode&0777)==0600);
        CHECK(!setgroups(0,NULL) && !setgid(1202) && !setuid(1201));
        CHECK(open(path,O_RDONLY)<0 && access(path,R_OK)<0);
        puts("ownership survived process exit; second user denied");return 0;
    }
    unlink(path);unlink(secret);
    int fd=open(secret,O_CREAT|O_EXCL|O_WRONLY,0600);
    CHECK(fd>=0 && write(fd,"root",4)==4 && !close(fd));
    CHECK(!chmod(secret,0600));
    CHECK(kill(getpid(),0)==0);
    CHECK(!setgroups(0,NULL) && !setgid(1102) && !setuid(1101));
    CHECK(getuid()==1101 && geteuid()==1101 && getgid()==1102 && getegid()==1102);
    CHECK(setuid(0)==-1 && errno==EPERM);
    CHECK(open(secret,O_RDONLY)<0 && chmod(secret,0644)<0 && unlink(secret)<0);
    umask(077);
    fd=open(path,O_CREAT|O_EXCL|O_RDWR,0666);
    CHECK(fd>=0 && write(fd,"mine",4)==4 && !fstat(fd,&st));
    CHECK(st.st_uid==1101 && st.st_gid==1102 && (st.st_mode&0777)==0600);
    CHECK(!close(fd));
    puts("credentials, process creation modes and private files OK");return 0;
}
