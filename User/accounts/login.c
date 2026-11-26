#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "prompt.h"
#include "session.h"

extern char **environ;

static int conversation(int n, const struct pam_message **messages,
                        struct pam_response **result, void *unused) {
    (void)unused;
    if (n <= 0 || n > PAM_MAX_NUM_MSG) return PAM_CONV_ERR;
    struct pam_response *r = calloc(n, sizeof(*r));
    if (!r) return PAM_BUF_ERR;
    for (int i = 0; i < n; ++i) {
        switch (messages[i]->msg_style) {
        case PAM_PROMPT_ECHO_OFF:
            r[i].resp = read_secret(messages[i]->msg);
            if (r[i].resp) continue;
            break;
        case PAM_ERROR_MSG: case PAM_TEXT_INFO:
            fprintf(stderr, "%s\n", messages[i]->msg);
            continue;
        }
        for (int j = 0; j < i; ++j) discard_secret(r[j].resp);
        free(r);
        return PAM_CONV_ERR;
    }
    *result = r;
    return PAM_SUCCESS;
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2],"--gnome"))) {
        fputs("usage: login USER [--gnome]\n", stderr); return 2;
    }
    if (geteuid() || getuid()) { fputs("login requires the system console\n",stderr); return 1; }
    struct pam_conv conv = {conversation, NULL};
    pam_handle_t *pamh = NULL;
    int rc = pam_start_confdir("lixinos-login", argv[1], &conv, "/lib/pam", &pamh);
    if (rc == PAM_SUCCESS) rc = pam_authenticate(pamh, PAM_DISALLOW_NULL_AUTHTOK);
    if (rc == PAM_SUCCESS) rc = pam_acct_mgmt(pamh, 0);
    const void *name = NULL;
    if (rc == PAM_SUCCESS) rc = pam_get_item(pamh, PAM_USER, &name);
    char *user = rc == PAM_SUCCESS && name ? strdup(name) : NULL;
    if (pamh) pam_end(pamh, rc);
    if (!user) { fputs("Login failed\n",stderr); return 1; }
    return user_session(user, argc==3);
}
