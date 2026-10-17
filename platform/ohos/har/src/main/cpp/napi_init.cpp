// napi_init.cpp — C++ entry of the OHOS HAR.
//
// Exports hello() plus thin wrappers around the capability probe in
// exec/caps.cpp, so ArkTS can read or pin the same word the reference server
// uses.
#include <napi/native_api.h>

/* OH_LOG_INFO uses these two when it is expanded; without them the default
 * domain is 0 and the tag is NULL, which is not a useful hilog record. */
#ifndef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#endif
#ifndef LOG_TAG
#define LOG_TAG "IEC_LOGGER"
#endif
#include <hilog/log.h>

#include "exec/caps.h"
#include "logger/logger.h"

namespace {

// The ArkTS shape shared by setCaps() and probeCaps(). packed is the four-digit
// octal value; the other fields are its digits.
napi_value MakeCaps(napi_env env, uint64_t packed) {
    const HostCapabilities caps = UnpackCaps(packed);

    napi_value obj = nullptr;
    napi_create_object(env, &obj);

    napi_value value = nullptr;
    napi_create_double(env, static_cast<double>(packed), &value);
    napi_set_named_property(env, obj, "packed", value);

    napi_create_int32(env, caps.fork, &value);
    napi_set_named_property(env, obj, "fork", value);
    napi_create_int32(env, caps.exec, &value);
    napi_set_named_property(env, obj, "exec", value);
    napi_create_int32(env, caps.jit, &value);
    napi_set_named_property(env, obj, "jit", value);
    napi_create_int32(env, caps.path, &value);
    napi_set_named_property(env, obj, "path", value);

    return obj;
}

napi_value Hello(napi_env env, napi_callback_info /*info*/) {
    napi_value ret = nullptr;
    const char* msg = "Hello from iec/platform/ohos/har (native)!";
    napi_create_string_utf8(env, msg, NAPI_AUTO_LENGTH, &ret);
    return ret;
}

// setCaps(cap: number): Caps
// Pins the capability word; the returned object is what it unpacked to.
napi_value SetCaps(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    if (argc < 1) {
        napi_throw_type_error(env, nullptr, "setCaps(cap) expects one number");
        return nullptr;
    }

    double value = 0;
    if (napi_get_value_double(env, argv[0], &value) != napi_ok) {
        napi_throw_type_error(env, nullptr, "setCaps(cap): cap must be a number");
        return nullptr;
    }

    const uint64_t packed = static_cast<uint64_t>(value);
    setCaps(packed);
    return MakeCaps(env, packed);
}

// probeCaps(): Caps
// Runs the probes and returns their packed answer.
napi_value ProbeCaps(napi_env env, napi_callback_info /*info*/) {
    return MakeCaps(env, ProbeCapabilities());
}

napi_value Init(napi_env env, napi_value exports) {
    // The logger has no OHOS dependency of its own. Register hilog as a custom
    // sink here, where the NAPI/hilog libraries are already linked.
    Logger::GetInstance()->SetCustomWriter([](int level, const std::string& line) {
        if (level >= 2) {
            OH_LOG_ERROR(LOG_APP, "%{public}s", line.c_str());
        } else if (level == 1) {
            OH_LOG_WARN(LOG_APP, "%{public}s", line.c_str());
        } else {
            OH_LOG_INFO(LOG_APP, "%{public}s", line.c_str());
        }
    });

    napi_property_descriptor desc[] = {
        {"hello", nullptr, Hello, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setCaps", nullptr, SetCaps, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"probeCaps", nullptr, ProbeCaps, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}

}  // namespace

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
