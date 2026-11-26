#pragma once
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int wait_session(pid_t pid) {
    int status;
    while (waitpid(pid,&status,0)<0) if (errno!=EINTR) return 1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

/* The caller authenticates first. Permanently drop every group and identity
 * before loading the user's shell, desktop settings or environment. */
static int user_session(const char *user,int graphical) {
    struct passwd *pw=getpwnam(user);
    if (!pw || !pw->pw_uid || pw->pw_uid<1000) {
        fputs("A regular desktop account is required\n",stderr); return 1;
    }
    uid_t uid=pw->pw_uid; gid_t gid=pw->pw_gid;
    char *home=strdup(pw->pw_dir), *shell=strdup(pw->pw_shell);
    if (!home || !shell || initgroups(user,gid) || setgid(gid) || setuid(uid) ||
        getuid()!=uid || geteuid()!=uid || getgid()!=gid || getegid()!=gid) {
        perror("login: establish credentials"); return 1;
    }
    umask(077);
    if (chdir(home) || clearenv() || setenv("HOME",home,1) || setenv("USER",user,1) ||
        setenv("LOGNAME",user,1) || setenv("SHELL",shell,1) || setenv("PATH","/bin",1) ||
        setenv("LANG","C.UTF-8",1) || setenv("TERM","xterm",1)) {
        perror("login: initialize session"); return 1;
    }
    printf("Starting session for %s (uid %lu)\n",user,(unsigned long)geteuid());
    fflush(stdout);
    char *args[]={graphical ? "/bin/startgnome" : shell,NULL};
    pid_t child;
    int error=posix_spawn(&child,args[0],NULL,NULL,args,environ);
    if (error) { errno=error; perror("login: start session"); return 1; }
    return wait_session(child);
}
