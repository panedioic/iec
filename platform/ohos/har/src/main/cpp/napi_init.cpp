// napi_init.cpp — C++ entry of the OHOS HAR.
//
// Exports hello() plus thin wrappers around the capability probe in
// exec/caps.cpp, so ArkTS can read or pin the same word the reference server
// uses.
#include <napi/native_api.h>
#include <sys/stat.h>

#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/* OH_LOG_INFO uses these two when it is expanded; without them the default
 * domain is 0 and the tag is NULL, which is not a useful hilog record. */
#ifndef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#endif
#ifndef LOG_TAG
#define LOG_TAG "IEC_LOGGER"
#endif
#include <hilog/log.h>

#include "common/ca_bundle.h"
#include "common/constant.h"
#include "exec/caps.h"
#include "exec/exec_server.h"
#include "logger/logger.h"
#include "tty/tty_server.h"

namespace {

// The ArkTS shape shared by setCaps() and probeCaps(). packed is the five-digit
// hexadecimal value; the other fields are its digits.
napi_value MakeCaps(napi_env env, uint64_t packed) {
    const HostCapabilities caps = UnpackCaps(packed);

    napi_value obj = nullptr;
    napi_create_object(env, &obj);

    napi_value value = nullptr;
    napi_create_double(env, static_cast<double>(packed), &value);
    napi_set_named_property(env, obj, "packed", value);

    napi_create_int32(env, caps.chmod, &value);
    napi_set_named_property(env, obj, "chmod", value);
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

// Reads one optional string argument. Missing, undefined and null all mean "use
// the default". Returns false after throwing when the argument is not a string.
bool ReadOptionalString(napi_env env, napi_value value, std::string& out) {
    napi_valuetype type = napi_undefined;

    if (napi_typeof(env, value, &type) != napi_ok) return false;
    if (type == napi_undefined || type == napi_null) return true;
    if (type != napi_string) {
        napi_throw_type_error(
            env, nullptr, "startIecServer: socket paths must be strings");
        return false;
    }

    size_t len = 0;
    if (napi_get_value_string_utf8(env, value, nullptr, 0, &len) != napi_ok)
        return false;
    std::vector<char> buf(len + 1, '\0');
    if (napi_get_value_string_utf8(env, value, buf.data(), buf.size(), &len) != napi_ok)
        return false;
    out.assign(buf.data(), len);
    return true;
}

// The two services are started and stopped as a pair, so one lock covers both
// pointers. The host may create and destroy the ability more than once, which is
// why start is idempotent and stop is safe to call when nothing is running.
std::mutex g_server_lock;
std::unique_ptr<TtyServer> g_tty_server;
std::unique_ptr<ExecServer> g_exec_server;

// Caller holds g_server_lock. Each pointer is null unless every step that
// produced it succeeded, so this is also the rollback for a partly started pair.
void StopServersLocked() {
    if (g_tty_server) {
        g_tty_server->stopAndJoin();
        g_tty_server.reset();
    }
    if (g_exec_server) {
        g_exec_server->stopAndJoin();
        g_exec_server.reset();
    }
}

// The sandbox paths in constant.h point at directories the application owns but
// that are not necessarily created yet. bind() would fail with ENOENT; create
// the parent, and let bind() report anything else.
void EnsureParentDir(const std::string& path) {
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0) return;
    mkdir(path.substr(0, slash).c_str(), 0755);
}

// startIecServer(ttyPath?: string, execPath?: string): boolean
napi_value StartServer(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    std::string tty_path, exec_path;
    if (argc > 0 && !ReadOptionalString(env, argv[0], tty_path)) return nullptr;
    if (argc > 1 && !ReadOptionalString(env, argv[1], exec_path)) return nullptr;

    bool started = false;
    {
        std::lock_guard<std::mutex> lock(g_server_lock);

        if (g_tty_server && g_exec_server) {
            started = true;
        } else {
            // A half pair cannot survive a successful start; clear it before
            // replacing it, or the running half would leak.
            if (g_tty_server || g_exec_server) StopServersLocked();

            const std::string tty = tty_path.empty() ? TTY_SOCK_PATH : tty_path;
            const std::string exe = exec_path.empty() ? EXEC_SOCK_PATH : exec_path;
            EnsureParentDir(tty);
            EnsureParentDir(exe);

            try {
                auto tty_srv = std::make_unique<TtyServer>(tty);
                auto exec_srv = std::make_unique<ExecServer>(exe);

                // Both sockets are up before either thread starts: a client must
                // never see one service up and the other still missing. The
                // locals below are destroyed on the way out of the catch, and
                // their destructors join any thread that did start.
                tty_srv->start();
                exec_srv->start();
                tty_srv->startThread();
                exec_srv->startThread();

                g_tty_server = std::move(tty_srv);
                g_exec_server = std::move(exec_srv);
                started = true;
                Logger::GetInstance()->LogI(
                    "[iec] server started: tty=%s exec=%s", tty.c_str(), exe.c_str());
            } catch (const std::exception& e) {
                Logger::GetInstance()->LogE("[iec] start failed: %s", e.what());
                StopServersLocked();
            }
        }
    }

    napi_value ret = nullptr;
    napi_get_boolean(env, started, &ret);
    return ret;
}

// stopIecServer(): boolean
// Stops both services and joins their threads. Returns true when a running
// server was stopped and false when none was running.
napi_value StopServer(napi_env env, napi_callback_info /*info*/) {
    bool was_running = false;
    {
        std::lock_guard<std::mutex> lock(g_server_lock);
        was_running = static_cast<bool>(g_tty_server) || static_cast<bool>(g_exec_server);
        StopServersLocked();
        if (was_running) Logger::GetInstance()->LogI("[iec] server stopped");
    }

    napi_value ret = nullptr;
    napi_get_boolean(env, was_running, &ret);
    return ret;
}

// installCaBundle(dir?: string): boolean
// Writes the CA bundle embedded in the library to <dir>/cacert.pem. Missing
// arguments default to the directory git reads under the fake root.
napi_value InstallCaBundle(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    std::string dir;
    if (argc > 0 && !ReadOptionalString(env, argv[0], dir)) return nullptr;
    if (dir.empty()) dir = std::string(FAKE_SYSROOT) + "/usr/share/git-core";

    const bool ok = IecInstallCaBundle(dir.c_str());
    if (!ok) {
        Logger::GetInstance()->LogW("[iec] CA bundle: cannot write to %s",
                                    dir.c_str());
    }

    napi_value ret = nullptr;
    napi_get_boolean(env, ok, &ret);
    return ret;
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

    // The bundle is written once here so it is in place before the first shell
    // can start git. installCaBundle() stays exported for callers that want to
    // refresh it after upgrading the embedded copy.
    if (!IecInstallCaBundle(FAKE_SYSROOT "/usr/share/git-core")) {
        Logger::GetInstance()->LogW("[iec] CA bundle: cannot write at init");
    }

    napi_property_descriptor desc[] = {
        {"hello", nullptr, Hello, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setCaps", nullptr, SetCaps, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"probeCaps", nullptr, ProbeCaps, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"startIecServer", nullptr, StartServer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopIecServer", nullptr, StopServer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"installCaBundle", nullptr, InstallCaBundle, nullptr, nullptr, nullptr, napi_default, nullptr},
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
