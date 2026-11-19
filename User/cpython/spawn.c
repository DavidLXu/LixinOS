#include "../console.h"
#include "runtime.h"
#include <Python.h>
#include <errno.h>
#include <string.h>

int lx_child_descriptor(int fd, int standard);
int lx_record_child(int pid);
static PyObject *spawn(PyObject *self, PyObject *args) {
    PyObject *argv, *sequence;
    const char *path, *cwd;
    int descriptors[3];
    if (!PyArg_ParseTuple(args, "ssOiii", &path, &cwd, &argv, &descriptors[0], &descriptors[1],
                          &descriptors[2]))
        return NULL;
    sequence = PySequence_Fast(argv, "expected argument sequence");
    if (!sequence)
        return NULL;
    Py_ssize_t argc = PySequence_Fast_GET_SIZE(sequence);
    unsigned char *mail = (void *)0x443d8000;
    size_t used = 0;
    uint64_t flags = 0;
    if (!argc || argc > 512) {
        errno = E2BIG;
        goto failed;
    }
    for (int i = 0; i < 3; i++) {
        int raw = lx_child_descriptor(descriptors[i], i);
        if (raw < 0) {
            errno = ENOTSUP;
            goto failed;
        }
        flags |= (uint64_t)raw << (i * 8);
    }
    for (Py_ssize_t i = -2; i < argc; i++) {
        Py_ssize_t n;
        const char *text;
        if (i < 0) {
            text = i == -2 ? cwd : path;
            n = strlen(text);
        } else {
            text = PyUnicode_AsUTF8AndSize(PySequence_Fast_GET_ITEM(sequence, i), &n);
            if (!text) {
                Py_DECREF(sequence);
                return NULL;
            }
            if (memchr(text, 0, n)) {
                errno = EINVAL;
                goto failed;
            }
        }
        if (used + n + 1 > 32768) {
            errno = E2BIG;
            goto failed;
        }
        memcpy(mail + used, text, n + 1);
        used += n + 1;
    }
    long pid = console_call(50, ((uint64_t)used << 32) | flags);
    if (pid < 0) {
        errno = ENOEXEC;
        goto failed;
    }
    lx_record_child(pid);
    Py_DECREF(sequence);
    return PyLong_FromLong(pid);
failed:
    Py_DECREF(sequence);
    return PyErr_SetFromErrnoWithFilename(PyExc_OSError, path);
}
static PyMethodDef methods[] = {{"spawn", spawn, METH_VARARGS, NULL}, {NULL, NULL, 0, NULL}};
static struct PyModuleDef module = {PyModuleDef_HEAD_INIT, "_lixin_process", NULL, -1, methods};
PyMODINIT_FUNC PyInit__lixin_process(void) { return PyModule_Create(&module); }
