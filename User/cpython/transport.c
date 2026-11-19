// pip's HTTP adapter uses the same authenticated TLS transport as the browser.
#include <Python.h>
#include "../tcc/lixin.h"
#include "../links/tls.h"
#include "../console.h"

struct connection { int fd; struct tls_connection *tls; };

static void release(PyObject *capsule) {
    struct connection *c = PyCapsule_GetPointer(capsule, "lixin.http");
    tls_free(c->tls);
    lx_tcp_close(c->fd);
    PyMem_Free(c);
}

static int plain_transfer(int fd, char *data, int length, int write) {
    for (int i = 0; i < 1500; i++) {
        int n = write ? lx_tcp_write(fd, data, length) : lx_tcp_read(fd, data, length);
        if (n != LX_TCP_AGAIN) return n;
        if (console_call(43, 8) < 0) break;
    }
    return -1;
}

static PyObject *request(PyObject *self, PyObject *args) {
    const char *host, *data;
    Py_ssize_t length;
    int port, secure;
    if (!PyArg_ParseTuple(args, "siiy#", &host, &port, &secure, &data, &length)) return NULL;
    struct connection *c = PyMem_Calloc(1, sizeof(*c));
    if (!c) return PyErr_NoMemory();
    c->fd = lx_tcp_connect(host, port);
    if (c->fd < 0) {
        PyMem_Free(c);
        return PyErr_Format(PyExc_OSError, "could not connect to %s:%d", host, port);
    }
    PyObject *capsule = PyCapsule_New(c, "lixin.http", release);
    if (!capsule) { lx_tcp_close(c->fd); PyMem_Free(c); return NULL; }
    const char *error = NULL;
    if (secure) {
        c->tls = tls_open(c->fd, host);
        error = c->tls ? tls_send(c->tls, data, length) : "TLS setup failed";
    } else {
        while (length) {
            int n = plain_transfer(c->fd, (char *)data, length > 4096 ? 4096 : (int)length, 1);
            if (n <= 0) { error = "TCP write failed"; break; }
            data += n; length -= n;
        }
    }
    if (error) { PyErr_SetString(PyExc_OSError, error); Py_DECREF(capsule); return NULL; }
    return capsule;
}

static PyObject *receive(PyObject *self, PyObject *args) {
    PyObject *capsule;
    char data[4096];
    int size;
    if (!PyArg_ParseTuple(args, "Oi", &capsule, &size)) return NULL;
    struct connection *c = PyCapsule_GetPointer(capsule, "lixin.http");
    if (!c) return NULL;
    if (size < 0 || size > sizeof(data)) return PyErr_Format(PyExc_ValueError, "invalid read size");
    int n = c->tls ? tls_receive(c->tls, data, size) : plain_transfer(c->fd, data, size, 0);
    if (n < 0) return PyErr_Format(PyExc_OSError, "%s", c->tls ? tls_error(c->tls) : "TCP read failed");
    return PyBytes_FromStringAndSize(data, n);
}
static PyMethodDef methods[] = {{"request", request, METH_VARARGS, NULL},
    {"receive", receive, METH_VARARGS, NULL}, {NULL, NULL, 0, NULL}};
static struct PyModuleDef module = {PyModuleDef_HEAD_INIT, "_lixin_http", NULL, -1, methods};
PyMODINIT_FUNC PyInit__lixin_http(void) { return PyModule_Create(&module); }
