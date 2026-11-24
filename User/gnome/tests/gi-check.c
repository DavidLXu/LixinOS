#include <girepository/girepository.h>
#include "static-modules.h"
#include <stdio.h>

static int lixin_probe_twice(int value)
{
    return 2 * value;
}

static const struct lx_module_symbol symbols[] = {
    {"lixin_probe_twice", lixin_probe_twice},
    {0}
};

const struct lx_module *lx_static_modules(void)
{
    static const struct lx_module modules[] = {
        {"liblixin-probe.so.0", symbols},
        {0}
    };
    return modules;
}

int main(void)
{
    GIRepository *repo = gi_repository_new();
    GError *error = NULL;
    GIBaseInfo *info = NULL;
    GIArgument argument = {.v_int = 21}, result = {0};
    int status = 1;
    gi_repository_prepend_search_path(repo, "/lib/gnome");
    if (!gi_repository_require(repo, "LixinProbe", "1.0", 0, &error))
        goto done;
    info = gi_repository_find_by_name(repo, "LixinProbe", "twice");
    if (!info ||
        !gi_function_info_invoke((GIFunctionInfo *)info, &argument, 1, NULL, 0, &result, &error))
        goto done;
    if (result.v_int == 42) {
        puts("GI typelib resolved and invoked a static symbol");
        status = 0;
    }

done:
    if (status) fprintf(stderr, "GI probe failed: %s\n", error ? error->message : "unexpected result");
    g_clear_error(&error);
    if (info) gi_base_info_unref(info);
    g_object_unref(repo);
    return status;
}
