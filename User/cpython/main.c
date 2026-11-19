#include <Python.h>

// Both kernels install the same pure-Python library and use the same EL0 ABI.
PyMODINIT_FUNC PyInit__lixin_http(void);
PyMODINIT_FUNC PyInit__ssl(void);
PyMODINIT_FUNC PyInit__lixin_process(void);
int main(int argc, char **argv) {
    if (PyImport_AppendInittab("_lixin_http", PyInit__lixin_http) < 0) return 1;
    if (PyImport_AppendInittab("_ssl", PyInit__ssl) < 0) return 1;
    if (PyImport_AppendInittab("_lixin_process", PyInit__lixin_process) < 0) return 1;
    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    config.site_import = 0;
    config.write_bytecode = 0;
    config.use_environment = 1;
    config.install_signal_handlers = 1;
    config.module_search_paths_set = 1;
    PyStatus status = PyConfig_SetBytesArgv(&config, argc, argv);
    if (!PyStatus_Exception(status))
        status = PyConfig_SetString(&config, &config.home, L"/");
    if (!PyStatus_Exception(status))
        status = PyWideStringList_Append(&config.module_search_paths, L"/lib/python3.14");
    if (!PyStatus_Exception(status))
        status = PyWideStringList_Append(&config.module_search_paths, L"/lib/python3.14/site-packages");
    if (!PyStatus_Exception(status))
        status = PyConfig_SetString(&config, &config.executable, L"/bin/python3");
    if (!PyStatus_Exception(status))
        status = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    if (PyStatus_Exception(status))
        Py_ExitStatusException(status);
    return Py_RunMain();
}
