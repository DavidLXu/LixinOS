#define _GNU_SOURCE
#include <gdm/gdm-client.h>
#include <gio/gio.h>
#include "../../accounts/prompt.h"
#include "../../accounts/session.h"

int gdm_manage(gboolean console);
#define SERVICE "gdm-password"

struct ui { GMainLoop *loop; GdmUserVerifier *verifier; gboolean started; };
static void password(GdmUserVerifier *v,const char *service,const char *question,struct ui *ui) {
    (void)question;
    char *answer=read_secret("Password: ");
    GError *error=NULL;
    if (!answer || !gdm_user_verifier_call_answer_query_sync(v,service,answer,NULL,&error))
        g_main_loop_quit(ui->loop);
    discard_secret(answer);
    if (error) {fprintf(stderr,"gdm greeter: %s\n",error->message);g_error_free(error);}
}
static void failed(GdmUserVerifier *v,const char *svc,struct ui *ui) {
    (void)v;(void)svc;fputs("Login failed\n",stderr);g_main_loop_quit(ui->loop);
}
static void opened(GdmGreeter *g,const char *svc,const char *id,struct ui *ui) {
    (void)id;
    ui->started=gdm_greeter_call_start_session_when_ready_sync(g,svc,TRUE,NULL,NULL);
    g_main_loop_quit(ui->loop);
}
static int console_greeter(void) {
    struct ui ui={0};
    GError *error=NULL;
    GdmClient *client=gdm_client_new();
    GdmGreeter *greeter=gdm_client_get_greeter_sync(client,NULL,&error);
    if (greeter) ui.verifier=gdm_client_get_user_verifier_sync(client,NULL,&error);
    if (!ui.verifier) goto out;
    ui.loop=g_main_loop_new(NULL,FALSE);
    g_signal_connect(ui.verifier,"secret-info-query",G_CALLBACK(password),&ui);
    g_signal_connect(ui.verifier,"verification-failed",G_CALLBACK(failed),&ui);
    g_signal_connect(greeter,"session-opened",G_CALLBACK(opened),&ui);
    fputs("Username: ",stderr);fflush(stderr);
    char user[128];size_t n=0;
    for (;;) {
        int c=get_byte();
        if (c=='\r' || c=='\n') break;
        if (c==3 || c==4 || c<=0 || n+1==sizeof(user)) goto out;
        if (c==8 || c==127) {if(n){--n;fputs("\b \b",stderr);}continue;}
        if (c>=32 && c<127) {user[n++]=c;fputc(c,stderr);}
    }
    user[n]=0;fputc('\n',stderr);
    if (gdm_user_verifier_call_begin_verification_for_user_sync(ui.verifier,SERVICE,user,NULL,&error))
        g_main_loop_run(ui.loop);
out:
    if(error){fprintf(stderr,"gdm greeter: %s\n",error->message);g_error_free(error);}
    g_clear_object(&greeter);g_clear_object(&ui.verifier);g_clear_object(&client);
    if(ui.loop)g_main_loop_unref(ui.loop);
    return ui.started?0:1;
}
static int greeter_session(int graphical) {
    struct passwd *pw=getpwnam("gdm");
    if (!pw || !pw->pw_uid) return 1;
    uid_t uid=pw->pw_uid; gid_t gid=pw->pw_gid;
    char *address=strdup(getenv("DBUS_SYSTEM_BUS_ADDRESS")?:"");
    if (!address || !*address || setgroups(0,NULL) || setgid(gid) || setuid(uid) ||
        geteuid()!=uid || getegid()!=gid || clearenv() ||
        setenv("DBUS_SYSTEM_BUS_ADDRESS",address,1) || setenv("PATH","/bin",1) ||
        setenv("USER","gdm",1) || setenv("LOGNAME","gdm",1) || setenv("LANG","C.UTF-8",1)) return 1;
    char home[]="/tmp/lx-greeter-XXXXXX";
    if (!mkdtemp(home) || setenv("HOME",home,1) || chdir(home)) return 1;
    umask(077);
    if (!graphical) return console_greeter();
    if (setenv("LIXINOS_GREETER","1",1)) return 1;
    char *args[]={"/bin/startgnome","--greeter",NULL};
    pid_t pid;
    return posix_spawn(&pid,args[0],NULL,NULL,args,environ)?1:wait_session(pid);
}
int main(int argc,char **argv) {
    if (getuid() || geteuid()) {fputs("gdm requires the system console\n",stderr);return 1;}
    if(argc==1) return gdm_manage(FALSE);
    if(argc==2 && !strcmp(argv[1],"--console")) return gdm_manage(TRUE);
    if(argc==2 && !strcmp(argv[1],"--console-greeter")) return greeter_session(0);
    if(argc==2 && !strcmp(argv[1],"--graphical-greeter")) return greeter_session(1);
    if(argc==3 && !strcmp(argv[1],"--user-shell")) return user_session(argv[2],0);
    if(argc==3 && !strcmp(argv[1],"--user-desktop")) return user_session(argv[2],1);
    fputs("usage: gdm [--console]\n",stderr);return 2;
}
