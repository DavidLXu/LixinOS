/* A single-seat LixinOS backend for the upstream GDM Manager, Greeter and
 * UserVerifier interfaces. Password verification uses the system PAM stack. */
#define _GNU_SOURCE
#include <gio/gio.h>
#include <security/pam_appl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "gdm-manager-glue.h"
#include "gdm-client-glue.h"
#include "../../accounts/session.h"

#define SERVICE "gdm-password"
#define SESSION_PATH "/org/gnome/DisplayManager/Session"
#define MANAGER_PATH "/org/gnome/DisplayManager/Manager"
#define MANAGER_NAME "org.gnome.DisplayManager"

struct seat {
    GMainLoop *loop;
    GDBusServer *server;
    GdmUserVerifier *verifier;
    GdmGreeter *greeter;
    GPtrArray *peers;
    char *name;
    pid_t frontend, peer_pid;
    uid_t greeter_uid;
    gboolean authenticated, start;
    guint failures;
    gint64 retry_at;
};

static gboolean refuse(GDBusMethodInvocation *call,const char *why) {
    g_dbus_method_invocation_return_error_literal(call,G_DBUS_ERROR,G_DBUS_ERROR_ACCESS_DENIED,why);
    return TRUE;
}

static gboolean open_session(GdmManager *manager,GDBusMethodInvocation *call,struct seat *s) {
    gdm_manager_complete_open_session(manager,call,g_dbus_server_get_client_address(s->server));
    return TRUE;
}
static gboolean reauthenticate(GdmManager *manager,GDBusMethodInvocation *call,
                               const char *user,void *unused) {
    (void)manager;(void)user;(void)unused;
    return refuse(call,"No running session for reauthentication");
}
static gboolean register_session(GdmManager *manager,GDBusMethodInvocation *call,
                                GVariant *info,void *unused) {
    (void)info; (void)unused;
    gdm_manager_complete_register_session(manager,call); return TRUE;
}
static void reset(struct seat *s) {
    s->authenticated=FALSE;
    g_clear_pointer(&s->name,g_free);
}
static gboolean begin(GdmUserVerifier *v,GDBusMethodInvocation *call,const char *service,
                      const char *user,struct seat *s) {
    if (strcmp(service,SERVICE) || !user || !*user || strlen(user)>255 ||
        g_get_monotonic_time()<s->retry_at) return refuse(call,"Verification unavailable");
    reset(s);
    s->name=g_strdup(user);
    gdm_user_verifier_complete_begin_verification_for_user(v,call);
    gdm_user_verifier_emit_conversation_started(v,SERVICE);
    if (getenv("LIXINOS_GDM_TRACE")) fputs("GDM password requested\n",stderr);
    gdm_user_verifier_emit_secret_info_query(v,SERVICE,"Password");
    return TRUE;
}
static int password_reply(int n,const struct pam_message **msg,struct pam_response **out,void *data) {
    if (n<1 || n>PAM_MAX_NUM_MSG) return PAM_CONV_ERR;
    struct pam_response *r=calloc(n,sizeof(*r));
    if (!r) return PAM_BUF_ERR;
    for (int i=0;i<n;++i) {
        if (msg[i]->msg_style==PAM_TEXT_INFO || msg[i]->msg_style==PAM_ERROR_MSG) continue;
        if (msg[i]->msg_style==PAM_PROMPT_ECHO_OFF && (r[i].resp=strdup(data))) continue;
        for (int j=0;j<i;++j) if (r[j].resp) {
            explicit_bzero(r[j].resp,strlen(r[j].resp));free(r[j].resp);
        }
        free(r);return PAM_CONV_ERR;
    }
    *out=r;return PAM_SUCCESS;
}
static gboolean answer(GdmUserVerifier *v,GDBusMethodInvocation *call,const char *service,
                       const char *answer,struct seat *s) {
    if (!s->name || s->authenticated || strcmp(service,SERVICE) || strlen(answer)>255)
        return refuse(call,"No pending password query");
    struct pam_conv conv={password_reply,(void *)answer};
    pam_handle_t *pam=NULL;
    int rc=pam_start_confdir("lixinos-login",s->name,&conv,"/lib/pam",&pam);
    if (rc==PAM_SUCCESS) rc=pam_authenticate(pam,PAM_DISALLOW_NULL_AUTHTOK);
    if (rc==PAM_SUCCESS) rc=pam_acct_mgmt(pam,0);
    const void *verified=NULL;
    if (rc==PAM_SUCCESS) rc=pam_get_item(pam,PAM_USER,&verified);
    struct passwd *pw=rc==PAM_SUCCESS && verified ? getpwnam(verified) : NULL;
    s->authenticated=pw && pw->pw_uid>=1000 && !strcmp(s->name,pw->pw_name);
    if (pam) pam_end(pam,rc);
    gdm_user_verifier_complete_answer_query(v,call);
    if (s->authenticated) {
        gdm_user_verifier_emit_verification_complete(v,SERVICE);
        /* GDM uses an empty logind ID for seats without a logind session. */
        gdm_greeter_emit_session_opened(s->greeter,SERVICE,"");
    } else {
        reset(s);
        if (s->failures<5) ++s->failures;
        s->retry_at=g_get_monotonic_time()+G_USEC_PER_SEC*s->failures;
        if (getenv("LIXINOS_GDM_TRACE")) fputs("GDM authentication denied\n",stderr);
        gdm_user_verifier_emit_problem(v,SERVICE,"Login failed");
        gdm_user_verifier_emit_verification_failed(v,SERVICE);
        gdm_user_verifier_emit_conversation_stopped(v,SERVICE);
    }
    return TRUE;
}
static gboolean cancel(GdmUserVerifier *v,GDBusMethodInvocation *call,struct seat *s) {
    reset(s);gdm_user_verifier_complete_cancel(v,call);
    gdm_user_verifier_emit_reset(v);return TRUE;
}
static gboolean extensions(GdmUserVerifier *v,GDBusMethodInvocation *call,
                           const char *const *names,void *unused) {
    (void)names;(void)unused;
    gdm_user_verifier_complete_enable_extensions(v,call);return TRUE;
}
static gboolean timed(GdmGreeter *g,GDBusMethodInvocation *call,void *unused) {
    (void)unused;gdm_greeter_complete_get_timed_login_details(g,call,FALSE,"",0);return TRUE;
}
static gboolean select_session(GdmGreeter *g,GDBusMethodInvocation *call,const char *name,void *unused) {
    (void)unused;
    if (strcmp(name,"gnome") && strcmp(name,"gnome-wayland")) return refuse(call,"Unknown session");
    gdm_greeter_complete_select_session(g,call);return TRUE;
}
static gboolean start(GdmGreeter *g,GDBusMethodInvocation *call,const char *svc,gboolean ready,struct seat *s) {
    if (!s->authenticated || strcmp(svc,SERVICE)) return refuse(call,"Authentication required");
    gdm_greeter_complete_start_session_when_ready(g,call);
    if (ready) {s->start=TRUE;g_main_loop_quit(s->loop);}
    return TRUE;
}
/* Libgdm may open separate peer connections for its verifier and greeter
 * proxies. Only the designated seat process may use either connection. */
static gboolean connect_peer(GDBusServer *server,GDBusConnection *peer,struct seat *s) {
    (void)server;
    GCredentials *cred=g_dbus_connection_get_peer_credentials(peer);
    if (!cred || g_credentials_get_unix_user(cred,NULL)!=s->greeter_uid) return FALSE;
    pid_t pid=g_credentials_get_unix_pid(cred,NULL);
    if (pid<=0 || (s->peer_pid && pid!=s->peer_pid)) return FALSE;
    s->peer_pid=pid;
    if (!g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(s->verifier),peer,SESSION_PATH,NULL) ||
        !g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(s->greeter),peer,SESSION_PATH,NULL)) return FALSE;
    g_ptr_array_add(s->peers,g_object_ref(peer));return TRUE;
}
static gboolean child_state(void *data) {
    struct seat *s=data;int status;
    if (waitpid(s->frontend,&status,WNOHANG)==0) return G_SOURCE_CONTINUE;
    s->frontend=0;g_main_loop_quit(s->loop);return G_SOURCE_REMOVE;
}
static int make_bus(const char *address,pid_t *pid) {
    char *option=g_strdup_printf("--address=%s",address);
    char *args[]={"/bin/dbus-daemon","--nofork","--config-file=/lib/gdm/system.conf",option,NULL};
    int rc=posix_spawn(pid,args[0],NULL,NULL,args,environ);
    g_free(option);return rc;
}
static void stop(pid_t *pid) {
    if (*pid>0) {kill(*pid,SIGKILL);wait_session(*pid);*pid=0;}
}
int gdm_manage(gboolean console) {
    struct seat s={0};
    struct passwd *account=getpwnam("gdm");
    if (!account || account->pw_uid==0) { fputs("Missing greeter account\n",stderr);return 1; }
    s.greeter_uid=account->pw_uid;
    gid_t gid=account->pw_gid;
    char directory[]="/tmp/lx-gdm-XXXXXX";
    if (!mkdtemp(directory) || chown(directory,0,gid) || chmod(directory,0710)) return 1;
    char *bus=g_strdup_printf("unix:abstract=%s-bus",strrchr(directory,'/')+1);
    char *peer=g_strdup_printf("unix:abstract=%s-session",strrchr(directory,'/')+1);
    pid_t daemon=0;
    GError *error=NULL;
    GDBusConnection *system=NULL;
    GdmManager *manager=NULL;
    char *guid=g_dbus_generate_guid();
    g_autoptr(GVariant) reply=NULL;
    int rc=1;
    const char *step="start system bus";
    if (make_bus(bus,&daemon)) goto done;
    for (unsigned i=0;i<100 && !system;++i) {
        g_clear_error(&error);
        system=g_dbus_connection_new_for_address_sync(bus,G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT|
                G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,NULL,NULL,&error);
        if (!system) usleep(50000);
    }
    if (!system) goto done;
    if (setenv("DBUS_SYSTEM_BUS_ADDRESS",bus,1)) goto done;
    step="acquire display manager name";
    reply=g_dbus_connection_call_sync(system,"org.freedesktop.DBus","/org/freedesktop/DBus",
            "org.freedesktop.DBus","RequestName",g_variant_new("(su)",MANAGER_NAME,4),
            G_VARIANT_TYPE("(u)"),0,5000,NULL,&error);
    if (!reply) goto done;
    guint owner=0;g_variant_get(reply,"(u)",&owner);
    if (owner!=1) goto done;
    s.loop=g_main_loop_new(NULL,FALSE);
    s.peers=g_ptr_array_new_with_free_func(g_object_unref);
    step="create greeter socket";
    s.server=g_dbus_server_new_sync(peer,G_DBUS_SERVER_FLAGS_NONE,guid,NULL,NULL,&error);
    if (!s.server) goto done;
    manager=gdm_manager_skeleton_new();
    s.verifier=gdm_user_verifier_skeleton_new();s.greeter=gdm_greeter_skeleton_new();
    gdm_manager_set_version(manager,"49.0");
#define ON(object,signal,fn) g_signal_connect(object,signal,G_CALLBACK(fn),&s)
    ON(manager,"handle-open-session",open_session);
    ON(manager,"handle-register-session",register_session);
    ON(manager,"handle-open-reauthentication-channel",reauthenticate);
    ON(s.verifier,"handle-begin-verification-for-user",begin);
    ON(s.verifier,"handle-answer-query",answer);
    ON(s.verifier,"handle-cancel",cancel);
    ON(s.verifier,"handle-enable-extensions",extensions);
    ON(s.greeter,"handle-get-timed-login-details",timed);
    ON(s.greeter,"handle-select-session",select_session);
    ON(s.greeter,"handle-start-session-when-ready",start);
    ON(s.server,"new-connection",connect_peer);
    step="export display manager";
    if (!g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(manager),system,MANAGER_PATH,&error)) goto done;
    g_dbus_server_start(s.server);
    char *args[]={"/bin/gdm",console?"--console-greeter":"--graphical-greeter",NULL};
    if (posix_spawn(&s.frontend,args[0],NULL,NULL,args,environ)) goto done;
    g_timeout_add(250,child_state,&s);
    g_main_loop_run(s.loop);
    rc=s.start?0:1;
done:
    if (error) {fprintf(stderr,"gdm: %s: %s\n",step,error->message);g_clear_error(&error);}
    if (s.server) g_dbus_server_stop(s.server);
    /* The foreground process tree owns the compositor and its private bus.
     * Reap that tree before handing the canvas to the new user. */
    stop(&s.frontend);
    g_clear_object(&manager);g_clear_object(&s.verifier);g_clear_object(&s.greeter);
    if (s.peers) g_ptr_array_unref(s.peers);
    g_clear_object(&s.server);g_clear_object(&system);stop(&daemon);
    if (s.loop) g_main_loop_unref(s.loop);
    rmdir(directory);
    g_free(bus);g_free(peer);g_free(guid);
    if (rc==0 && s.name) {
        /* Use a fresh process for the privilege transition. GIO has
         * background threads and must not inherit the user's session. */
        char *args[]={"/bin/gdm",console?"--user-shell":"--user-desktop",s.name,NULL};
        pid_t pid;
        rc=posix_spawn(&pid,args[0],NULL,NULL,args,environ)?1:wait_session(pid);
    }
    reset(&s);return rc;
}
