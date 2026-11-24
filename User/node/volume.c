// Expose the guest's revision-checked filesystem transaction to Node tools.
#include <node_api.h>
#include <stdint.h>
#include <string.h>

static napi_value batch(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3], result;
    uint32_t op;
    char *mail = (char *)0x443d7000;
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok ||
        argc != 3 || napi_get_value_uint32(env, argv[0], &op) != napi_ok)
        goto invalid;
    for (int i = 0; i < 2; ++i) {
        size_t length, copied;
        char *path = mail + 513 * i;
        if (napi_get_value_string_utf8(env, argv[i + 1], NULL, 0, &length) != napi_ok ||
            length > 512 ||
            napi_get_value_string_utf8(env, argv[i + 1], path, 513, &copied) != napi_ok ||
            copied != length || strlen(path) != length)
            goto invalid;
    }
    register long x0 asm("x0") = op;
    register long x8 asm("x8") = 45;
    asm volatile("svc #0" : "+r"(x0) : "r"(x8) : "x9", "x10", "cc", "memory");
    if (x0 < 0) {
        napi_throw_error(env, "EIO", mail);
        return NULL;
    }
    napi_create_int32(env, x0, &result);
    return result;
invalid:
    napi_throw_type_error(env, NULL, "expected a transaction operation and two guest paths");
    return NULL;
}

static napi_value init(napi_env env, napi_value exports) {
    napi_value method;
    napi_create_function(env, "batch", NAPI_AUTO_LENGTH, batch, NULL, &method);
    napi_set_named_property(env, exports, "batch", method);
    return exports;
}

__attribute__((constructor)) static void register_volume(void) {
    static napi_module module = {.nm_version = NAPI_MODULE_VERSION, .nm_modname = "lixinos_fs",
                                 .nm_register_func = init};
    napi_module_register(&module);
}
