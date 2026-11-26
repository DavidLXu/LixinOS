#define _GNU_SOURCE
#include <crypt.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <shadow.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "prompt.h"

static int replace_hash(const char *user, const char *hash) {
    int lock=open("/etc/.pwd.lock",O_CREAT|O_EXCL|O_WRONLY,0600);
    if (lock<0) return -1;
    FILE *in=fopen("/etc/shadow","r");
    if (!in) { close(lock); unlink("/etc/.pwd.lock"); return -1; }
    char tmp[]="/etc/.shadow-XXXXXX";
    int fd=mkstemp(tmp);
    FILE *out=fd<0 ? NULL : fdopen(fd,"w");
    char *line=NULL;
    size_t cap=0, len=strlen(user);
    int found=0, ok=0;
    if (out && !fchmod(fd,0600)) {
        while (getline(&line,&cap,in)>=0) {
            if (!strncmp(line,user,len) && line[len]==':') {
                char *rest=strchr(line+len+1,':');
                if (!rest || !(rest=strchr(rest+1,':')) || found++) break;
                if (fprintf(out,"%s:%s:%ld%s",user,hash,(long)(time(NULL)/86400),rest)<0) break;
            } else if (fputs(line,out)==EOF) break;
        }
        ok=found==1 && feof(in) && !ferror(in) && !ferror(out) && !fflush(out) && !fsync(fd);
    }
    if (line) { explicit_bzero(line,cap); free(line); }
    if (out) { if (fclose(out)) ok=0; } else if (fd>=0) close(fd);
    if (ok) ok=rename(tmp,"/etc/shadow")==0;
    if (fd>=0 && !ok) unlink(tmp);
    close(lock); unlink("/etc/.pwd.lock"); fclose(in);
    return ok ? 0 : -1;
}
int main(int argc,char **argv) {
    if (argc!=2 || getuid() || geteuid()) {
        fputs("usage from the administrator console: passwd USER\n",stderr); return 2;
    }
    if (!getpwnam(argv[1])) { fputs("Unknown account\n",stderr); return 1; }
    char *first=read_secret("New password: ");
    char *second=read_secret("Retype new password: ");
    unsigned char random[16];
    char salt[]="$6$rounds=100000$................$";
    const char alphabet[]="./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    int ok=first && second && *first && !strcmp(first,second) && !getentropy(random,sizeof(random));
    if (ok) {
        for (unsigned i=0;i<sizeof(random);++i) salt[17+i]=alphabet[random[i]&63];
        char *hash=crypt(first,salt);
        ok=hash && !strncmp(hash,"$6$",3) && !replace_hash(argv[1],hash);
        if (hash) explicit_bzero(hash,strlen(hash));
    }
    discard_secret(first); discard_secret(second);
    puts(ok ? "Password updated" : "Password update failed");
    return !ok;
}
