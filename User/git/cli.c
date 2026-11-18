// Local porcelain. Repository, index, diff, tree, commit and history operations
// are performed by libgit2 inside the guest process.
#include <git2.h>
#include <git2/sys/errors.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../console.h"
#include "platform.h"

static char confirmation[600];

static int fail(const char *message) {
    git_error_set_str(GIT_ERROR_INVALID, message);
    return -1;
}

static int head(git_commit **out, git_repository *repo) {
    git_reference *ref = NULL;
    int rc = git_repository_head(&ref, repo);
    if (rc == GIT_EUNBORNBRANCH) {
        *out = NULL;
        return 0;
    }
    if (!rc)
        rc = git_commit_lookup(out, repo, git_reference_target(ref));
    git_reference_free(ref);
    return rc;
}

static int status(git_repository *repo) {
    git_status_options opts = GIT_STATUS_OPTIONS_INIT;
    opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;
    git_status_list *list = NULL;
    int rc = git_status_list_new(&list, repo, &opts);
    if (rc)
        return rc;
    size_t n = git_status_list_entrycount(list);
    for (size_t i = 0; i < n; ++i) {
        const git_status_entry *e = git_status_byindex(list, i);
        unsigned f = e->status;
        const git_diff_delta *d = e->index_to_workdir ? e->index_to_workdir : e->head_to_index;
        char x = f & GIT_STATUS_INDEX_NEW        ? 'A'
                 : f & GIT_STATUS_INDEX_DELETED  ? 'D'
                 : f & GIT_STATUS_INDEX_MODIFIED ? 'M'
                                                 : ' ';
        char y = f & GIT_STATUS_WT_DELETED ? 'D' : f & GIT_STATUS_WT_MODIFIED ? 'M' : ' ';
        if (f & GIT_STATUS_WT_NEW)
            x = y = '?';
        printf("%c%c %s\n", x, y, d->new_file.path);
    }
    if (!n)
        puts("Working tree clean");
    git_status_list_free(list);
    return 0;
}

static int matches_operand(const char *file, const char *operand) {
    size_t len = strlen(operand);
    return !strcmp(file, operand) || (!strncmp(file, operand, len) && file[len] == '/');
}

static int select_operand(const char *file, const char *match, void *payload) {
    (void)match;
    return !matches_operand(file, payload); // Positive skips the entry.
}

static int add(git_repository *repo, int argc, char **argv) {
    git_index *idx = NULL;
    int rc = git_repository_index(&idx, repo);
    for (int i = 0; !rc && i < argc; ++i) {
        // The shell resolves operands before launch, relative to the worktree.
        char *path = argv[i];
        int all = !strcmp(path, ".");
        struct stat st;
        if (!all && stat(path, &st) != 0) {
            size_t j = 0, count = git_index_entrycount(idx);
            while (j < count && !matches_operand(git_index_get_byindex(idx, j)->path, path))
                ++j;
            if (j == count) {
                rc = fail("path does not exist in the working tree or index");
                break;
            }
        }
        if (!all && !git_index_get_bypath(idx, path, 0)) {
            int ignored;
            rc = git_ignore_path_is_ignored(&ignored, repo, path);
            if (!rc && ignored)
                rc = fail("path is ignored");
        }
        if (!rc)
            rc = git_index_add_all(idx, NULL, 0, all ? NULL : select_operand, path);
    }
    if (!rc)
        rc = git_index_write(idx);
    git_index_free(idx);
    return rc;
}

static int print_line(const git_diff_delta *d, const git_diff_hunk *h, const git_diff_line *line,
                      void *payload) {
    (void)d;
    (void)h;
    (void)payload;
    if (line->origin == '+' || line->origin == '-' || line->origin == ' ')
        putchar(line->origin);
    fwrite(line->content, 1, line->content_len, stdout);
    return 0;
}

static int diff(git_repository *repo, int staged) {
    git_commit *parent = NULL;
    git_tree *tree = NULL;
    git_diff *delta = NULL;
    int rc = 0;
    if (staged) {
        rc = head(&parent, repo);
        if (!rc && parent)
            rc = git_commit_tree(&tree, parent);
        if (!rc)
            rc = git_diff_tree_to_index(&delta, repo, tree, NULL, NULL);
    } else
        rc = git_diff_index_to_workdir(&delta, repo, NULL, NULL);
    if (!rc)
        rc = git_diff_print(delta, GIT_DIFF_FORMAT_PATCH, print_line, NULL);
    git_diff_free(delta);
    git_tree_free(tree);
    git_commit_free(parent);
    return rc;
}

static int commit(git_repository *repo, const char *message) {
    git_index *idx = NULL;
    git_commit *parent = NULL;
    git_tree *tree = NULL;
    git_signature *sig = NULL;
    git_oid tree_id, id;
    int rc = git_repository_index(&idx, repo);
    if (!rc)
        rc = git_index_write_tree(&tree_id, idx);
    if (!rc)
        rc = head(&parent, repo);
    if (!rc && ((parent && git_oid_equal(&tree_id, git_commit_tree_id(parent))) ||
                (!parent && !git_index_entrycount(idx))))
        rc = fail("nothing staged to commit");
    if (!rc)
        rc = git_tree_lookup(&tree, repo, &tree_id);
    if (!rc)
        rc = git_signature_new(&sig, "LixinOS", "user@lixinos.local", time(NULL), 0);
    if (!rc) {
        const git_commit *parents[] = {parent};
        rc = git_commit_create(&id, repo, "HEAD", sig, sig, NULL, message, tree, parent ? 1 : 0,
                               parents);
        if (!rc)
            snprintf(confirmation, sizeof(confirmation), "%.12s %s\n", git_oid_tostr_s(&id),
                     message);
    }
    git_signature_free(sig);
    git_tree_free(tree);
    git_commit_free(parent);
    git_index_free(idx);
    return rc;
}

static int log_commits(git_repository *repo) {
    git_commit *c = NULL;
    int rc = head(&c, repo);
    if (!rc && !c)
        puts("No commits yet");
    while (!rc && c) {
        printf("commit %s\nAuthor: %s <%s>\n\n    %s\n\n", git_oid_tostr_s(git_commit_id(c)),
               git_commit_author(c)->name, git_commit_author(c)->email, git_commit_message(c));
        git_commit *next = NULL;
        if (git_commit_parentcount(c))
            rc = git_commit_parent(&next, c, 0);
        git_commit_free(c);
        c = next;
    }
    return rc;
}

__attribute__((section(".text.start"), noreturn)) void
_start(const char *initial_root, const char *initial_args, size_t len) {
    static char root[513], args[1024];
    if (!initial_root || strlen(initial_root) >= sizeof(root) || !len || len >= sizeof(args))
        finish(1);
    strcpy(root, initial_root);
    memcpy(args, initial_args, len);
    char *argv[256];
    int argc = 0;
    for (size_t pos = 0; pos < len && argc < 256;) {
        argv[argc++] = args + pos;
        pos += strlen(args + pos) + 1;
    }
    lx_start(root);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    git_repository *repo = NULL;
    int rc = lx_libgit_init();
    if (!rc && argc == 1 && !strcmp(argv[0], "init")) {
        git_repository_init_options opts = GIT_REPOSITORY_INIT_OPTIONS_INIT;
        opts.initial_head = "main";
        rc = git_repository_init_ext(&repo, root, &opts);
        if (!rc)
            snprintf(confirmation, sizeof(confirmation), "Git repository ready in %s\n",
                     git_repository_path(repo));
    } else if (!rc) {
        rc = git_repository_open(&repo, root);
        if (!rc && argc == 1 && !strcmp(argv[0], "status"))
            rc = status(repo);
        else if (!rc && argc >= 2 && !strcmp(argv[0], "add"))
            rc = add(repo, argc - 1, argv + 1);
        else if (!rc && !strcmp(argv[0], "diff") &&
                 (argc == 1 ||
                  (argc == 2 && (!strcmp(argv[1], "--cached") || !strcmp(argv[1], "--staged")))))
            rc = diff(repo, argc == 2);
        else if (!rc && argc == 3 && !strcmp(argv[0], "commit") && !strcmp(argv[1], "-m") &&
                 *argv[2])
            rc = commit(repo, argv[2]);
        else if (!rc && argc == 1 && !strcmp(argv[0], "log"))
            rc = log_commits(repo);
        else if (!rc)
            rc = fail("usage: git init | status | add PATH... | diff [--staged] | commit -m "
                      "MESSAGE | log");
    }
    if (rc) {
        const git_error *err = git_error_last();
        if (err)
            fprintf(stderr, "git: %s\n", err->message);
    }
    git_repository_free(repo);
    if (!rc)
        lx_confirm(confirmation);
    finish(rc != 0);
}
