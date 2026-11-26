#include <node_api.h>
static napi_value sum(napi_env env, napi_callback_info info) {
    size_t n=2; napi_value v[2],ret; int32_t a,b;
    if (napi_get_cb_info(env,info,&n,v,0,0)!=napi_ok || n!=2 ||
        napi_get_value_int32(env,v[0],&a)!=napi_ok || napi_get_value_int32(env,v[1],&b)!=napi_ok) return 0;
    napi_create_int64(env,(int64_t)a+b,&ret);return ret;
}
NAPI_MODULE_INIT(){napi_value fn;napi_create_function(env,"add",3,sum,0,&fn);return fn;}
