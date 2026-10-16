// exec/exec_server.cpp — ExecServer implementation
#include "exec_server.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <thread>
#include <unordered_map>

// How a program is started. Which one applies is decided by ResolveKind() from
// the host capabilities below.
enum class LaunchKind {
    kAuto,          // nobody has decided yet
    kExec,          // plain fork + execve
    kSo,            // a shared object, entered through the loader
    kLoader,        // a loader is executed in the program's place
    kQemu_x86_64,   // emulated, qemu in the program's place
    kQemu_aarch64,
    kUnexecutable,  // no way to run this path
};

struct InheritedFd { 
    int source_fd; 
    int target_fd; 
};

struct SpawnRequest {
    std::string path;                       // The path of the executable file
    std::vector<std::string> argv;          // argv[0] is the command name
    std::vector<std::string> env;           // "K=V"
    std::string cwd;                        // empty is inherit
    LaunchKind kind_hint = LaunchKind::kAuto;   // kAuto = the caller did not decide
    std::vector<InheritedFd> stdio_fds;     // target ∈ {0,1,2}
    std::vector<InheritedFd> extra_fds;     // target > 2
    int         sid = 0;
    bool        sync_wait = true;           // A foreground terminal command always waits.
    std::string proc_name;                  // prctl(PR_SET_NAME)
    pid_t       caller_pid = 0;             // logic parent pid
};

struct SpawnResult {
    pid_t      pid = -1;
    LaunchKind kind = LaunchKind::kAuto;    // what the child was actually started as
    int        exit_code = -1;
    std::string error;
};

struct ProcessInfo {
    pid_t       pid = 0;
    pid_t       parent_pid = 0;
    LaunchKind  kind = LaunchKind::kAuto;
    std::string cmd;
    int64_t     start_time_ms = 0;
    bool        alive = true;
    int         exit_code = -1;
};

struct HostCapabilities {
    /* FORK Probe
     * 0: fork is unavailable on this device. 
     * 1: fork available. */
    int fork = 0;
    /* EXEC Probe
     * 0: exec is unavailable on this device. 
     * 1: exec is available in specific directories (i.e. /bin/sh), but unavailable in other path.
     * 2: exec is available */
    int exec = 0;
    /* JIT Probe
     * 0: None of the methods are applicable. Only qemu simulation can be used. 
     * 1: JIT can be achieved through the mprotect method. 
     * 2: memfd. 
     * 3: anon-rwx.*/
    int jit = 0;
    /* PATH Probe
     * 0: only app sandbox is reachable. 
     * 1: part of the system path (i.e. /bin/sh) is reachable.
     * 2: full-disk is reachable */
    int path = 0;
};
/* Probe results for this device: whether fork works, which paths can be exec'd,
 * how a JIT page can be obtained, and how much of the filesystem is reachable.
 * The fields below are hardcoded placeholders -- the probes that should fill them
 * in are not written yet -- and hold the values confirmed by hand on the target
 * device. Nothing may branch on them before the probes exist. */
static HostCapabilities g_caps = {1, 2, 3, 2};

// Every child this server started, keyed by pid. The spawn path writes it and the
// reaper in onTick() reads it, and both run on the thread that executes run(), so
// there is no second thread to race with and this deliberately has no lock.
static std::unordered_map<pid_t, ProcessInfo> procmap;

/* ------------------------------------------------------------------ */
/* Resolver                                                           */
/* ------------------------------------------------------------------ */

std::string ResolveFilePath(std::string path) {
    // todo: implement future.
    return path;
}

LaunchKind ResolveKind(std::string path) {
    // todo: implement future.
    if (g_caps.fork == 1 && g_caps.exec == 2)
        return LaunchKind::kExec;
    return LaunchKind::kUnexecutable;
}

/* ------------------------------------------------------------------ */
/* Common Helpers                                                     */
/* ------------------------------------------------------------------ */

// CLOCK_MONOTONIC rather than the wall clock: these stamps are only ever compared
// with each other, and a clock step must not make one process look older than the
// parent that started it.
static int64_t NowMillis() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------------ */
/* Routers for All Spawn Kind                                         */
/* ------------------------------------------------------------------ */

pid_t SpawnExec(const SpawnRequest& req) {
    // execve() takes plain C arrays, and a vector<string> is not one. The strings
    // themselves stay valid for the duration of the call, which is all that is
    // needed: a successful execve never comes back here.
    std::vector<char*> argv;
    std::vector<char*> envp;
    argv.reserve(req.argv.size() + 1);
    envp.reserve(req.env.size() + 1);
    for (const std::string& s : req.argv) argv.push_back(const_cast<char*>(s.c_str()));
    for (const std::string& s : req.env)  envp.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);
    envp.push_back(nullptr);

    // A non-empty cwd asks for the program to be started there; an empty one keeps
    // the directory the server was started in.
    if (!req.cwd.empty() && chdir(req.cwd.c_str()) != 0) return -1;

    execve(req.path.c_str(), argv.data(), envp.data());
    return -1;   // reached only when execve failed
}

/* ------------------------------------------------------------------ */
/* Overall Spawn and Terimate Function                                */
/* ------------------------------------------------------------------ */

// Exit code of a child that never got to run the program at all. 127 is the
// shell's "command not found", so it cannot be confused with a status the program
// produced itself.
constexpr int kExecFailedExit = 127;

SpawnResult Spawn(const SpawnRequest& req) {
    SpawnResult res;
    if (req.path.empty()) { res.error = "path is empty"; return res; }

    // kAuto means the caller left the decision to us. It is settled before the
    // fork so that both sides agree on what is being started.
    res.kind = req.kind_hint == LaunchKind::kAuto ? ResolveKind(req.path) : req.kind_hint;

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[exec] fork failed!\n");
        res.pid = -1;
        return res;
    } else if (pid == 0) {
        // Child. It must never return into the caller: the caller is the server
        // itself, so a child that walked back out of here would carry on as a
        // second server, competing for the same socket and running its own poll
        // loop. _exit() is the only way out, and it avoids flushing a second copy
        // of the parent's stdio buffers too.
        //
        // Only write(2) from here on. The parent runs one thread per service, so
        // at the moment of the fork another thread may hold a stdio lock that does
        // not exist in the child; fprintf would deadlock instead of printing.
        switch (res.kind) {
            case LaunchKind::kExec:
                SpawnExec(req);   /* returns only on failure */
                {
                    static const char msg[] = "[exec] exec failed\n";
                    if (write(STDERR_FILENO, msg, sizeof msg - 1)) {}
                }
                _exit(kExecFailedExit);
            default:
                // kSo, kLoader, kQemu_* and kUnexecutable have no router yet.
                {
                    static const char msg[] = "[exec] launch kind unsupported\n";
                    if (write(STDERR_FILENO, msg, sizeof msg - 1)) {}
                }
                _exit(kExecFailedExit);
        }
    }
    // parent
    ProcessInfo info;
    info.pid = pid;
    info.parent_pid = req.caller_pid > 0 ? req.caller_pid : getpid();
    info.kind = res.kind;
    info.cmd = req.path;
    info.start_time_ms = NowMillis();
    info.alive = true;
    procmap[info.pid] = info;

    // sync_wait is still not honoured: nothing here makes the caller wait for the
    // child. exit_code is filled in later, by the reaper in onTick(), which only
    // notices the exit on some later pass of the event loop.
    res.pid = pid;
    return res;
}

void Terminate(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    std::thread([pid]() {
        usleep(800 * 1000);
        if (kill(pid, 0) == 0) kill(pid, SIGKILL);
    }).detach();
}

/* ------------------------------------------------------------------ */
/* Exec Server                                                        */
/* ------------------------------------------------------------------ */

ExecServer::ExecServer(const std::string& path) : SockServer(path) {
    using namespace std::placeholders;
    registerHandler(OP_PING,       std::bind(&ExecServer::onPing,       this, _1, _2));
    registerHandler(OP_PONG,       std::bind(&ExecServer::onPong,       this, _1, _2));
    registerHandler(OP_EXECUTE,    std::bind(&ExecServer::onExecute,    this, _1, _2));
}

// Runs once per pass of the poll loop, whether or not anything arrived, which is
// what makes it the right place to notice children that exited while the server
// was idle: a request-driven handler would only run again on the next request,
// and by then a pile of zombies is already sitting there.
//
// Reaping here rather than in a SIGCHLD handler is deliberate. waitpid() is itself
// async-signal-safe, but the bookkeeping that has to follow it is not: updating
// procmap can allocate, and the allocator is not reentrant. The signal can also be
// delivered to any thread, including one that is in the middle of touching that
// map. Neither problem exists here -- this runs on the thread that runs run(), the
// same one that runs onExecute, so procmap needs no lock.
//
// waitpid(-1, ...) collects any child of this process, whoever started it, so this
// rests on exec being the only service that forks: the tty service forks nothing
// (forkpty() is done on the client side). If that ever changes, the new forker has
// to reap its own children, or its exit statuses would be consumed here. A pid
// that is not in procmap is skipped for the same reason rather than treated as an
// error.
void ExecServer::onTick() {
    // In a loop, because several children can have exited since the previous pass
    // and waitpid() reports them one at a time. WNOHANG so this can never stall
    // the loop on a child that is still running.
    for (;;) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        // 0 means children exist but none has exited yet, -1 that there are none
        // left or the call failed; both end the loop.
        if (pid <= 0) break;

        auto it = procmap.find(pid);
        if (it == procmap.end()) continue;
        it->second.alive = false;
        // A signal death has no exit code of its own, so it is reported the way a
        // shell reports it -- 128 + the signal number -- which keeps the field a
        // plain int and stops "killed by SIGTERM" from looking like an ordinary exit.
        // With WNOHANG and no WUNTRACED/WCONTINUED, waitpid() only ever returns
        // exits and signal deaths, so the last arm is unreachable and only there to
        // leave the field defined.
        it->second.exit_code = WIFEXITED(status)   ? WEXITSTATUS(status)
                             : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                                   : -1;
    }
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
    // The request is decoded into a SpawnRequest and goes no further; see the note
    // in front of the reply at the end of this function.
    SpawnRequest sr;
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
    sr.path = ResolveFilePath(file);
    sr.cwd = cwd;
    sr.kind_hint = ResolveKind(sr.path);
    sr.sid = 0; // todo: multisession in future.
    // The remaining fields keep their defaults: sync_wait is true (a foreground
    // command on a terminal always waits), and no caller pid or process name is
    // carried by the request yet.

    fprintf(stderr, "[exec] file=%s cwd=%s flags=0x%x argc=%u envc=%u fds=%zu\n",
            file.c_str(), cwd.empty() ? "(inherit)" : cwd.c_str(),
            flags, argc, envc, req.fds.size());

    for (uint32_t i = 0; i < argc; i++) {
        std::string a;
        if (!next("argv", a)) return false;
        fprintf(stderr, "[exec]   argv[%u] = %s\n", i, a.c_str());
        sr.argv.push_back(a);
    }
    for (uint32_t i = 0; i < envc; i++) {
        std::string e;
        if (!next("env", e)) return false;
        fprintf(stderr, "[exec]   env[%u] = %s\n", i, e.c_str());
        sr.env.push_back(e);
    }
    if (off != n) fprintf(stderr, "[exec] %zu trailing bytes ignored\n", n - off);

    // Wired up, so that a request now turns into a real process. Two things are
    // still missing, and both make the result less useful than it looks:
    //
    //   * fd inheritance: stdio_fds and extra_fds are filled by nothing and read
    //     by nothing, so the child comes up with the server's own stdin/stdout
    //     instead of the pty slave -- its output lands wherever the server was
    //     started from, not on a terminal.
    //   * the client side: the exec hook in exec/shim.c still ignores this reply
    //     and returns -1 unconditionally, so the caller is told the exec failed
    //     even when a process really started, and nothing waits for that process.
    SpawnResult res = Spawn(sr);
    if (res.pid < 0) {
        fprintf(stderr, "[exec] spawn failed: %s\n",
                res.error.empty() ? "unknown error" : res.error.c_str());
        return false;
    }
    fprintf(stderr, "[exec] spawned pid %d\n", (int)res.pid);
    put32(reply.body, static_cast<uint32_t>(res.pid));
    return true;
}
