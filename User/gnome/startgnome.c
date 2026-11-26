#define _GNU_SOURCE
#include <errno.h>
#include <spawn.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int configure_session(const char *runtime)
{
    const struct {
        const char *name;
        const char *value;
        int replace;
    } settings[] = {
        {"XDG_RUNTIME_DIR", runtime, 1},
        {"XDG_DATA_DIRS", "/lib/gnome/share", 0},
        {"XDG_SESSION_TYPE", "wayland", 1},
        {"XDG_CURRENT_DESKTOP", "GNOME", 1},
        {"GSETTINGS_SCHEMA_DIR", "/lib/gnome/schemas", 1},
        {"GSETTINGS_BACKEND", "keyfile", 0},
        {"FONTCONFIG_FILE", "/lib/gnome/fonts.conf", 0},
        {"XCURSOR_PATH", "/lib/gnome/share/icons", 0},
        {"LIBGWEATHER_LOCATIONS_PATH", "/lib/gnome/Locations.bin", 0},
        {"GSK_RENDERER", "cairo", 0},
    };
    for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); ++i) {
        if (setenv(settings[i].name, settings[i].value, settings[i].replace) < 0)
            return errno;
    }
    return 0;
}

static int start_input(const char *runtime,pid_t children[2])
{
    if (access("/bin/ibus-daemon",X_OK)<0 || access("/bin/ibus-engine-simple",X_OK)<0) return 0;
    struct sockaddr_un peer={.sun_family=AF_UNIX};
    if (snprintf(peer.sun_path,sizeof(peer.sun_path),"%s/ibus",runtime)>=(int)sizeof(peer.sun_path)) return ENAMETOOLONG;
    char address[144],address_file[128],option[156];
    snprintf(address,sizeof(address),"unix:path=%s",peer.sun_path);
    snprintf(address_file,sizeof(address_file),"%s/ibus.address",runtime);
    snprintf(option,sizeof(option),"--address=%s",address);
    if (setenv("IBUS_ADDRESS_FILE",address_file,1) || setenv("IBUS_COMPONENT_PATH","/lib/ibus/component",1)) return errno;
    char *daemon[]={"/bin/ibus-daemon","--single","--cache=none",option,NULL};
    int err=posix_spawn(&children[0],daemon[0],NULL,NULL,daemon,environ);
    if (err) return err;
    for (unsigned i=0;i<100;++i) {
        int fd=socket(AF_UNIX,SOCK_STREAM,0);
        int ready=fd>=0 && connect(fd,(void *)&peer,sizeof(peer))==0;
        if (fd>=0) close(fd);
        if (ready) {
            if (setenv("IBUS_ADDRESS",address,1) || setenv("GTK_IM_MODULE","ibus",0) ||
                setenv("LIXINOS_COMPOSE_KEY","F12",0)) return errno;
            char *engine[]={"/bin/ibus-engine-simple",NULL};
            err=posix_spawn(&children[1],engine[0],NULL,NULL,engine,environ);
            if (err) return err;
            char *select_engine[]={"/bin/ibus-input","--activate",NULL};
            pid_t setup;
            err=posix_spawn(&setup,select_engine[0],NULL,NULL,select_engine,environ);
            if (err) return err;
            int status;
            while (waitpid(setup,&status,0)<0) if (errno!=EINTR) return errno;
            return WIFEXITED(status) && WEXITSTATUS(status)==0 ? 0 : EIO;
        }
        usleep(20000);
    }
    return ETIMEDOUT;
}
static void stop_input(pid_t children[2],const char *runtime)
{
    for(int i=1;i>=0;--i) if(children[i]>0) {
        kill(children[i],SIGKILL);
        while(waitpid(children[i],NULL,0)<0 && errno==EINTR) {}
    }
    char path[128];snprintf(path,sizeof(path),"%s/ibus.address",runtime); unlink(path);
}

int main(int argc, char **argv)
{
    const char *name=strrchr(argv[0],'/');
    int input_only=!strcmp(name?name+1:argv[0],"ibus-run");
    int greeter=!input_only && argc==2 && !strcmp(argv[1],"--greeter");
    if (input_only && argc<2) {fputs("usage: ibus-run COMMAND [ARGUMENT ...]\n",stderr);return 2;}
    if (!input_only && argc!=1 && !greeter) return 2;
    char runtime[] = "/tmp/lx-gnome-XXXXXX";
    if (!mkdtemp(runtime)) {
        perror("startgnome: create runtime directory");
        return 1;
    }

    char *args[] = {"/bin/dbus-run-session", "--private-system-bus", "--",
                    "/bin/gnome-shell", "--lixinos-display", greeter?"--mode=gdm":NULL, NULL};
    pid_t child;
    int status = 0;
    int error = input_only ? 0 : configure_session(runtime);
    pid_t input[2]={0,0};
    if (!error && !greeter && !getenv("IBUS_ADDRESS")) {
        int ime_error=start_input(runtime,input);
        if (ime_error) {
            fprintf(stderr,"IBus: %s\n",strerror(ime_error));
            if (input_only) error=ime_error;
        }
    }
    if (!error) {
        char **command=input_only?argv+1:args;
        error=posix_spawnp(&child,command[0],NULL,NULL,command,environ);
    }
    if (!error) {
        pid_t result;
        do {
            result = waitpid(child, &status, 0);
        } while (result < 0 && errno == EINTR);
        if (result < 0)
            error = errno;
    }
    stop_input(input,runtime);
    rmdir(runtime);
    if (error) {
        fprintf(stderr, "startgnome: %s\n", strerror(error));
        return 1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
