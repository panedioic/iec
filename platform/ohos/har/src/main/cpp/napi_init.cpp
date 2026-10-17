// napi_init.cpp — C++ entry of the smoke-test HAR: exports hello().
#include <napi/native_api.h>

static napi_value Hello(napi_env env, napi_callback_info /*info*/) {
    napi_value ret = nullptr;
    const char* msg = "Hello from iec/platform/ohos/har (native)!";
    napi_create_string_utf8(env, msg, NAPI_AUTO_LENGTH, &ret);
    return ret;
}

static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"hello", nullptr, Hello, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}

static napi_module helloModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "iec_server",
    .nm_priv = nullptr,
    .reserved = { 0 },
};

extern "C" __attribute__((constructor)) void RegisterHelloModule(void) {
    napi_module_register(&helloModule);
}
