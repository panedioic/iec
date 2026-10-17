// exec/path_resolver.cpp — where a requested program is looked up.
//
// These two functions answer "what does this path mean on this host?":
// UnderFakeRoot rewrites an absolute path into the fake root when the PATH
// capability says only the app sandbox is reachable; ResolveFilePath applies
// POSIX PATH search to a bare name. Both are declared in exec_route.h.
#include "exec_route.h"

#include "caps.h"
#include "constant.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace {

bool IsExecutableFile(const std::string& path) {
    struct stat st;

    if (stat(path.c_str(), &st) != 0) return false;
    if (!S_ISREG(st.st_mode)) return false;
    return access(path.c_str(), X_OK) == 0;
}

std::string PathForRequest(const std::vector<std::string>& env) {
    for (const std::string& e : env)
        if (e.compare(0, 5, "PATH=") == 0) return e.substr(5);

    if (const char* own = getenv("PATH")) return own;

    std::string fallback;
    size_t n = confstr(_CS_PATH, nullptr, 0);
    if (n > 0) {
        fallback.resize(n);
        confstr(_CS_PATH, &fallback[0], n);
        if (fallback.back() == '\0') fallback.pop_back();
    }
    return fallback;
}

}  // namespace

std::string UnderFakeRoot(const std::string& path) {
    if (Caps().path != 0) return path;
    if (path.empty() || path[0] != '/') return path;
    return std::string(FAKE_SYSROOT) + path;
}

std::string ResolveFilePath(const std::string& path, const std::vector<std::string>& env,
                            const std::string& cwd) {
    if (path.find('/') != std::string::npos) return path;

    const std::string dirs = PathForRequest(env);
    size_t start = 0;

    for (;;) {
        const size_t colon = dirs.find(':', start);
        const size_t end = colon == std::string::npos ? dirs.size() : colon;
        const std::string dir = dirs.substr(start, end - start);
        // The search runs in the root the program will be started in, which is the fake
        // one where only the sandbox is reachable, so an absolute entry means inside it.
        const std::string in_root = UnderFakeRoot(dir);
        const std::string candidate =
            in_root.empty() ? (cwd.empty() ? path : cwd + "/" + path) : in_root + "/" + path;

        if (IsExecutableFile(candidate)) return candidate;
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return std::string();
}
