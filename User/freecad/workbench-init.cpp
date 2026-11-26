// Shared workbench libraries normally construct their globals when Python imports
// them. Preserve that ordering for the statically linked guest executable.
#include <Python.h>

using Constructor = void (*)();
#define WORKBENCH(name, dependencies)                          \
    extern "C" Constructor fc_##name##_begin[], fc_##name##_end[]; \
    extern "C" PyObject *__real_PyInit_##name();                \
    static void init_##name()                                  \
    {                                                          \
        static bool started;                                   \
        if (started) return;                                   \
        started = true;                                        \
        dependencies;                                          \
        for (auto ctor = fc_##name##_begin; ctor != fc_##name##_end; ++ctor) \
            (*ctor)();                                         \
    }                                                          \
    extern "C" PyObject *__wrap_PyInit_##name()                 \
    {                                                          \
        init_##name();                                         \
        return __real_PyInit_##name();                          \
    }

WORKBENCH(Materials, (void)0)
WORKBENCH(Measure, (void)0)
WORKBENCH(Part, init_Materials(); init_Measure())
WORKBENCH(Sketcher, init_Part())
WORKBENCH(MatGui, init_Materials())
WORKBENCH(MeasureGui, init_Measure())
WORKBENCH(PartGui, init_Part(); init_MatGui(); init_MeasureGui())
WORKBENCH(SketcherGui, init_Sketcher(); init_PartGui())

extern "C" {
PyObject *PyInit__coin();
PyObject *PyInit_Shiboken();
PyObject *PyInit_QtCore();
PyObject *PyInit_QtGui();
PyObject *PyInit_QtWidgets();
PyObject *PyInit_QtPrintSupport();
PyObject *PyInit_QtUiTools();
}

int registerGuestBindings()
{
    static struct _inittab bindings[] = {
        {"pivy._coin", PyInit__coin},
        {"shiboken6.Shiboken", PyInit_Shiboken},
        {"PySide6.QtCore", PyInit_QtCore},
        {"PySide6.QtGui", PyInit_QtGui},
        {"PySide6.QtWidgets", PyInit_QtWidgets},
        {"PySide6.QtPrintSupport", PyInit_QtPrintSupport},
        {"PySide6.QtUiTools", PyInit_QtUiTools},
        {nullptr, nullptr}
    };
    return PyImport_ExtendInittab(bindings);
}
