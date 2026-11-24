#ifndef LIXINOS_STATIC_MODULES_H
#define LIXINOS_STATIC_MODULES_H

struct lx_module_symbol {
    const char *name;
    void *address;
};

struct lx_module {
    const char *name;
    const struct lx_module_symbol *symbols;
};

/* The application supplies null-terminated tables at link time. Module names
   are the sonames from the GIR metadata or names requested by GModule. */
const struct lx_module *lx_static_modules(void);

#endif
