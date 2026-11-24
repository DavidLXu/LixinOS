/* GModule backend for libraries linked into a LixinOS application. */
#include "static-modules.h"

extern const struct lx_module *lx_static_modules(void) __attribute__((weak));
static char main_handle;

static gpointer
_g_module_open (const gchar  *file_name,
                gboolean      bind_lazy,
                gboolean      bind_local,
                GError      **error)
{
  const struct lx_module *module = lx_static_modules ? lx_static_modules () : NULL;
  const gchar *slash = strrchr (file_name, G_DIR_SEPARATOR);
  const gchar *name = slash ? slash + 1 : file_name;

  (void) bind_lazy;
  (void) bind_local;
  for (; module && module->name; module++)
    if (strcmp (module->name, name) == 0)
      return (gpointer) module;

  g_module_set_error ("Module is not linked into this application");
  g_set_error (error, G_MODULE_ERROR, G_MODULE_ERROR_FAILED,
               "Module is not linked into this application: %s", file_name);
  return NULL;
}

static gpointer
_g_module_self (void)
{
  return &main_handle;
}

static void
_g_module_close (gpointer handle)
{
  (void) handle;
}

static gpointer
_g_module_symbol (gpointer     handle,
                  const gchar *symbol_name)
{
  gboolean main = handle == &main_handle;
  const struct lx_module *module;

  if (main)
    module = lx_static_modules ? lx_static_modules () : NULL;
  else
    module = handle;

  for (; module && module->name; module++)
    {
      const struct lx_module_symbol *entry;
      for (entry = module->symbols; entry->name; entry++)
        if (strcmp (entry->name, symbol_name) == 0)
          return entry->address;
      if (!main)
        break;
    }

  g_module_set_error ("Symbol is not registered in this application");
  return NULL;
}
