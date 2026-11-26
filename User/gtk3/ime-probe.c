#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <ibus.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../desktop/window-client.h"
extern char **environ;

static gint processed, commits;
static GDBusMessage *observe(GDBusConnection *conn,GDBusMessage *msg,gboolean incoming,gpointer data) {
    (void)conn;(void)data;
    const char *name=g_dbus_message_get_member(msg);
    if (!incoming && g_strcmp0(name,"ProcessKeyEvent")==0) g_atomic_int_inc(&processed);
    if (incoming && g_strcmp0(name,"CommitText")==0) g_atomic_int_inc(&commits);
    return msg;
}

static int probe(int argc,char **argv) {
    g_setenv("GDK_BACKEND","broadway",TRUE);
    g_setenv("GSETTINGS_BACKEND","memory",TRUE);
    g_setenv("NO_AT_BRIDGE","1",TRUE);
    g_setenv("GTK_IM_MODULE","ibus",TRUE);
    g_setenv("IBUS_ENABLE_SYNC_MODE","0",TRUE);
    g_setenv("FONTCONFIG_FILE","/lib/gtk3/fonts.conf",TRUE);
    if (!gtk_init_check(&argc,&argv)) return 1;
    GtkWidget *window=gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GtkWidget *entry=gtk_entry_new();
    gtk_container_add(GTK_CONTAINER(window),entry);
    gtk_widget_show_all(window);
    gtk_window_set_focus(GTK_WINDOW(window),entry);
    GdkWindow *surface=gtk_widget_get_window(entry);
    GtkIMContext *context=gtk_im_multicontext_new();
    gtk_im_multicontext_set_context_id(GTK_IM_MULTICONTEXT(context),"ibus");
    gtk_im_context_set_client_window(context,surface);
    const char *chosen=gtk_im_multicontext_get_context_id(GTK_IM_MULTICONTEXT(context));
    if (g_strcmp0(chosen,"ibus")) return 2;
    g_object_set(entry,"im-module","ibus",NULL);
    gtk_widget_grab_focus(entry);
    GdkEvent *focus=gdk_event_new(GDK_FOCUS_CHANGE);
    focus->focus_change.window=g_object_ref(surface);
    focus->focus_change.in=TRUE;
    gtk_widget_send_focus_change(entry,focus);
    gdk_event_free(focus);
    for (int i=0;i<40;++i) {while(g_main_context_iteration(NULL,FALSE)){} g_usleep(10000);}
    IBusBus *bus=ibus_bus_new_async_client();
    GDBusConnection *conn=ibus_bus_get_connection(bus);
    if (!conn || !ibus_bus_is_connected(bus)) { fputs("GTK IBus connection unavailable\n",stderr); return 3; }
    g_dbus_connection_add_filter(conn,observe,NULL,NULL);
    int ready=open(argv[2],O_WRONLY);
    if (ready<0 || write(ready,"R",1)!=1) return 4;
    close(ready);
    for (int i=0;i<500 && strcmp(gtk_entry_get_text(GTK_ENTRY(entry)),"é");++i) {
        while(g_main_context_iteration(NULL,FALSE)) {}
        g_usleep(10000);
    }
    const char *text=gtk_entry_get_text(GTK_ENTRY(entry));
    fprintf(stderr,"GtkEntry via %s: %s\n",chosen,text);
    int calls = g_atomic_int_get(&processed), received = g_atomic_int_get(&commits);
    fprintf(stderr,"IBus key calls %d, commits %d\n",calls,received);
    return strcmp(text,"é")!=0 || calls!=6 || received!=1;
}
int main(int argc,char **argv) {
    if(argc==3 && !strcmp(argv[1],"--widget"))return probe(argc,argv);
    int fds[2],sink=open("/dev/null",O_WRONLY);
    if(sink<0 || pipe(fds))return 1;
    posix_spawn_file_actions_t io;
    posix_spawn_file_actions_init(&io);
    posix_spawn_file_actions_adddup2(&io,fds[0],0);
    posix_spawn_file_actions_adddup2(&io,sink,1);
    char sync[]="/tmp/gtk-ime-ready-XXXXXX";
    int ready=mkstemp(sync);
    if(ready<0)return 1;
    char *args[]={"/bin/gtk-ime-check","--widget",sync,NULL};
    pid_t child;
    if(posix_spawn(&child,args[0],&io,NULL,args,environ))return 1;
    close(fds[0]);close(sink);
    WindowEvent initial={WINDOW_CLIENT_CONFIGURE,300,120,0};
    if(write(fds[1],&initial,sizeof(initial))!=sizeof(initial))return 1;
    char signal=0;
    for(int i=0;i<500 && read(ready,&signal,1)!=1;++i) usleep(10000);
    close(ready);
    unlink(sync);
    if(signal!='R')return 1;
    static const char sequence[]="\033[24~'e";
    for(size_t i=0;i<sizeof(sequence)-1;++i) {
        WindowEvent key={WINDOW_CLIENT_KEY,0,0,sequence[i]};
        if(write(fds[1],&key,sizeof(key))!=sizeof(key))return 1;
        usleep(25000);
    }
    int status=0;
    if(waitpid(child,&status,0)!=child)return 1;
    close(fds[1]);
    return WIFEXITED(status)?WEXITSTATUS(status):1;
}
