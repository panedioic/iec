// exec/exec_server.cpp — ExecServer implementation
#include "exec_server.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>

ExecServer::ExecServer(const std::string& path) : SockServer(path) {
    using namespace std::placeholders;
    registerHandler(OP_PING,       std::bind(&ExecServer::onPing,       this, _1, _2));
    registerHandler(OP_PONG,       std::bind(&ExecServer::onPong,       this, _1, _2));
    registerHandler(OP_EXECUTE,    std::bind(&ExecServer::onExecute,    this, _1, _2));
}

bool ExecServer::onPing(const Packet& req, Reply& reply) {
    reply.body = req.body;
    return true;
}

bool ExecServer::onPong(const Packet& req, Reply& reply) {
    // The client never sends a pong; this only occupies the op number.
    (void)req;
    (void)reply;
    return true;
}

bool ExecServer::onExecute(const Packet& req, Reply& reply) {
    // Layout: [flags][argc][envc] then NUL-terminated strings in the order
    // file, cwd, argv..., env...; see the encoder in exec/shim.c for the detail.
    const uint8_t* b = req.body.data();
    size_t n = req.body.size();
    uint32_t flags, argc, envc;
    size_t off = 12;

    if (n < 12) {
        fprintf(stderr, "[exec] request too short: %zu bytes\n", n);
        return false;
    }
    flags = get32(b);
    argc = get32(b + 4);
    envc = get32(b + 8);

    // Walks one NUL-terminated string. The length is checked as it goes, so a
    // truncated or hostile request cannot run off the end of the buffer.
    auto next = [&](const char* what, std::string& out) -> bool {
        size_t start = off;
        while (off < n && b[off] != '\0') off++;
        if (off >= n) {
            fprintf(stderr, "[exec] %s: string is not terminated\n", what);
            return false;
        }
        out.assign(reinterpret_cast<const char*>(b + start), off - start);
        off++;
        return true;
    };

    std::string file, cwd;
    if (!next("file", file)) return false;
    if (!next("cwd", cwd)) return false;

    fprintf(stderr, "[exec] file=%s cwd=%s flags=0x%x argc=%u envc=%u fds=%zu\n",
            file.c_str(), cwd.empty() ? "(inherit)" : cwd.c_str(),
            flags, argc, envc, req.fds.size());

    for (uint32_t i = 0; i < argc; i++) {
        std::string a;
        if (!next("argv", a)) return false;
        fprintf(stderr, "[exec]   argv[%u] = %s\n", i, a.c_str());
    }
    for (uint32_t i = 0; i < envc; i++) {
        std::string e;
        if (!next("env", e)) return false;
        fprintf(stderr, "[exec]   env[%u] = %s\n", i, e.c_str());
    }
    if (off != n) fprintf(stderr, "[exec] %zu trailing bytes ignored\n", n - off);

    // Nothing is executed yet. Doing it needs the child to be forked and handed
    // the fds that arrived with the request, so the reply is a placeholder pid.
    put32(reply.body, 0);
    return true;
}
