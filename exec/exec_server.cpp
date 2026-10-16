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
    std::vector<InheritedFd> extra_fds;     // target > 2; no request fills this yet
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
    // The raw status word waitpid() returned, which is what OP_WAIT answers with:
    // it is the only form that still carries _how_ the child ended, so the client
    // can tell an ordinary exit from a signal death. Meaningful once alive is
    // false; both fields are filled at the same moment, from the same call.
    int         status = 0;
    // The same ending rendered for a human -- an exit code, or 128 + signal for a
    // signal death. Kept beside the raw word rather than instead of it because the
    // raw word alone cannot be read at a glance in a log; nothing branches on it.
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

// Every child this server started, keyed by pid. The spawn path writes it, and the
// request handlers (onWait, onSignal) read it alongside the reaper in onTick().
// All of them run on the thread that executes run(), so there is no second thread
// to race with and this deliberately has no lock; anything added here has to stay
// on that thread, or the first thing it needs is a mutex.
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
/* Inherited fds                                                      */
/* ------------------------------------------------------------------ */

// A temporary copy of a source fd is taken at or above this number. It only has
// to sit above every target the request may name (0, 1 and 2), so that taking
// the copy cannot itself overwrite a descriptor that is still to be filled in.
constexpr int kTempFdFloor = 10;

// Moves the fds the request carried into the descriptors the request named.
// Reports whether that worked; a false is fatal for the child.
//
// This runs in the child of fork(), before execve(), where only async-signal-safe
// calls are allowed: dup2(), close() and fcntl() are on that list, printf() and
// malloc() are not. That is why a failure comes back as a return value instead of
// a message, and why the caller reports it with write(2).
//
// The fds that arrived with the request but were not named as targets need no
// closing. The server's poll loop receives them with MSG_CMSG_CLOEXEC, so every
// one of them is close-on-exec and the execve() below drops it; the descriptors
// dup2() creates in their place do not carry that flag -- dup2() clears
// FD_CLOEXEC on the new fd, which is POSIX behaviour -- and that is exactly what
// the child wants, so nothing has to clear or set it by hand here.
bool InstallInheritedFds(const SpawnRequest& req) {
    const size_t n = req.stdio_fds.size();
    int temp[kMaxFds];
    size_t i, j;

    for (i = 0; i < n; i++) temp[i] = -1;

    // dup2(src, tgt) closes tgt before it duplicates onto it, so an entry whose
    // source is also the target of another entry would lose that source before it
    // is used: with {5 -> 0, 0 -> 1}, the first dup2 destroys the fd the second
    // one is supposed to read. The traffic this shim sends cannot look like that
    // -- the sources are the descriptors recvmsg() just installed in the server,
    // far above the 0/1/2 targets -- but the mapping arrives in a request, so it
    // is checked instead of assumed: a source that some other entry targets is
    // duplicated to a free high fd first, and the targets are then filled from
    // the copies.
    for (i = 0; i < n; i++) {
        for (j = 0; j < n; j++)
            if (j != i && req.stdio_fds[j].target_fd == req.stdio_fds[i].source_fd) break;
        if (j == n) continue;
        temp[i] = fcntl(req.stdio_fds[i].source_fd, F_DUPFD, kTempFdFloor);
        if (temp[i] < 0) return false;
    }

    for (i = 0; i < n; i++) {
        int source = temp[i] >= 0 ? temp[i] : req.stdio_fds[i].source_fd;
        if (dup2(source, req.stdio_fds[i].target_fd) < 0) return false;
    }

    // Each target holds its own copy of the descriptor now, so the temporary
    // names can go; leaving them would be a leak into the exec'd program.
    for (i = 0; i < n; i++)
        if (temp[i] >= 0) close(temp[i]);
    return true;
}

/* ------------------------------------------------------------------ */
/* Overall Spawn and Terimate Function                                */
/* ------------------------------------------------------------------ */

// Exit code of a child that never got to run the program at all. 127 is the
// shell's "command not found", so it cannot be confused with a status the program
// produced itself.
constexpr int kExecFailedExit = 127;

// Puts the child's signal mask back the way a program is entitled to start: empty.
//
// The child is forked from an event loop thread, and those run with SIGINT and
// SIGTERM blocked -- main.cpp blocks them so that a second signal cannot cut the
// server's own shutdown short (see the sigsuspend there). A mask is inherited
// across fork() and survives execve(), so without this every program started here
// would begin life unable to take those two at all: a ^C forwarded to it (see
// onSignal) would sit pending in the kernel until something unblocked it, which
// for most programs is never, and the signal would look like it had been ignored.
//
// Only the mask needs this. execve() gives every caught signal its default
// disposition on its own, and the server ignores none of them. A disposition of
// SIG_IGN such as nohup leaves behind is inherited by design, and is not the
// server's to override.
//
// sigprocmask() is on the list of calls a child of fork() may make.
void ClearInheritedSignalMask() {
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, nullptr);
}

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
        ClearInheritedSignalMask();

        // The stdio the caller sent comes next: it has to be in place before the
        // program is entered, and a failure here means the child would run with
        // the server's own stdin/stdout, which is the very thing fd inheritance
        // exists to avoid -- so it is reported rather than ignored.
        if (!InstallInheritedFds(req)) {
            static const char msg[] = "[exec] inherited fd setup failed\n";
            if (write(STDERR_FILENO, msg, sizeof msg - 1)) {}
            _exit(kExecFailedExit);
        }
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

    // sync_wait is still not honoured here: this call returns to the client as soon
    // as the fork is done. The waiting is done by the client, which polls OP_WAIT
    // for this pid once it has the reply. The entry above starts out alive, and its
    // exit is filled in later -- by the reaper in onTick(), or by onWait() for a
    // client that is polling harder than the event loop passes.
    res.pid = pid;
    return res;
}

// Ends a child this server started, gently first and with SIGKILL if it is still
// there 800 ms later. Nothing calls it: the signal a stand-in forwards now goes
// straight out through kill() in onSignal() below, because there is nothing to
// escalate to -- the signal has already been chosen by whoever sent it. It is
// left here as the place a policy would go if one is ever wanted (a deadline on
// a child that ignored what it was sent, say).
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
    registerHandler(OP_WAIT,       std::bind(&ExecServer::onWait,       this, _1, _2));
    registerHandler(OP_SIGNAL,     std::bind(&ExecServer::onSignal,     this, _1, _2));
}

// Notes how a child ended. This is one function rather than two copies because
// the same waitpid() result reaches procmap from two directions: onTick() reaps a
// child before anybody asked about it, and onWait() reaps one on demand because a
// client is waiting for it. Both have to leave the entry saying the same thing.
static void RecordExit(ProcessInfo& info, int status) {
    info.alive = false;
    // The raw word, which is what OP_WAIT hands back: it is the only form that
    // still says whether the child exited or was killed by a signal.
    info.status = status;
    // The same ending made readable for a log. A signal death has no exit code of
    // its own, so it is rendered the way a shell renders it -- 128 + the signal
    // number -- which stops "killed by SIGTERM" from looking like an ordinary exit.
    // With WNOHANG and no WUNTRACED/WCONTINUED, waitpid() only ever returns exits
    // and signal deaths, so the last arm is unreachable and only there to leave the
    // field defined.
    info.exit_code = WIFEXITED(status)   ? WEXITSTATUS(status)
                   : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                         : -1;
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
//
// One ending this hook does not catch: a stand-in killed outright, with SIGKILL or
// by the machine going down. No handler runs in the client for SIGKILL, so nothing
// is ever asked of this server and the child stays here as a live entry with
// nobody left to collect it. What it would take is a check in this same loop --
// the only place with the whole table in hand -- that drops the children of a
// client that is gone: for every entry still alive, test whether the client that
// asked for it is still there, and end the child if not. The pid to test is
// ProcessInfo::parent_pid, which Spawn() fills from SpawnRequest::caller_pid and
// which nothing reads yet; note that it needs the request to carry the client's
// pid first, because no request does today and the field therefore holds this
// server's own pid. Not implemented.
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
        RecordExit(it->second, status);
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
    // Layout: [flags][argc][envc][fd_count] then fd_count target words, then
    // NUL-terminated strings in the order file, cwd, argv..., env...; see the
    // encoder in exec/shim.c for the detail.
    const uint8_t* b = req.body.data();
    size_t n = req.body.size();
    uint32_t flags, argc, envc, fd_count;
    size_t off;

    if (n < 16) {
        fprintf(stderr, "[exec] request too short: %zu bytes\n", n);
        return false;
    }
    flags = get32(b);
    argc = get32(b + 4);
    envc = get32(b + 8);
    fd_count = get32(b + 12);

    // One target word per fd that arrived with the request, in the order the
    // framework received them, so the two lists have to be the same length. A
    // disagreement cannot be resolved -- whichever way the targets were then
    // matched up, they might be off by one, and the child would get the caller's
    // stderr on its stdout -- so the request is refused rather than guessed at.
    if (fd_count != req.fds.size()) {
        fprintf(stderr, "[exec] fd_count %u does not match %zu attached fds\n",
                fd_count, req.fds.size());
        return false;
    }
    if (n < 16 + (size_t)fd_count * 4) {
        fprintf(stderr, "[exec] request too short for %u fd targets\n", fd_count);
        return false;
    }

    // The strings start after the header and the target list, so every offset
    // below moves with fd_count.
    off = 16 + (size_t)fd_count * 4;
    for (uint32_t i = 0; i < fd_count; i++) {
        uint32_t target = get32(b + 16 + (size_t)i * 4);
        // Only the three standard descriptors are inherited for now. A target
        // beyond them would need a launch path that does not exist yet, and
        // taking the entry while ignoring it would silently drop what the caller
        // asked for, so an unknown target is an invalid request.
        if (target > STDERR_FILENO) {
            fprintf(stderr, "[exec] fd target %u is not a standard descriptor\n", target);
            return false;
        }
        sr.stdio_fds.push_back(InheritedFd{req.fds[i], static_cast<int>(target)});
    }

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

    fprintf(stderr, "[exec] file=%s cwd=%s flags=0x%x argc=%u envc=%u fds=%zu",
            file.c_str(), cwd.empty() ? "(inherit)" : cwd.c_str(),
            flags, argc, envc, req.fds.size());
    for (size_t i = 0; i < sr.stdio_fds.size(); i++)
        fprintf(stderr, " %d->%d", sr.stdio_fds[i].source_fd, sr.stdio_fds[i].target_fd);
    fputc('\n', stderr);

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

    // A request turns into a real process whose stdio is the caller's: the fds
    // that came with the request are dup2'd onto the targets the request named, in
    // the child, before it execs (see InstallInheritedFds). The pid goes back in
    // the reply, and it is the whole point of the reply: the caller has no other
    // way to name this child, and OP_WAIT is how it collects the exit status.
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

// Answers whether one child has finished, and with what status. It is the second
// half of the exec handshake: the client's stand-in polls this until the state
// word flips, then leaves with the same status, so that its caller's waitpid()
// sees what the real program did (see the wait loop in exec/shim.c).
//
// Only a pid this server really spawned may be waited for, and procmap is the
// authority on that, not waitpid() itself: waitpid() would happily answer for any
// child of this process, and the request names the pid as a plain unsigned word,
// so a request for -1 -- which collects an arbitrary child -- or for a child of
// some future feature has to be refused here rather than looked up and passed on.
bool ExecServer::onWait(const Packet& req, Reply& reply) {
    if (req.body.size() < 4) {
        fprintf(stderr, "[exec] wait request too short: %zu bytes\n", req.body.size());
        return false;
    }
    pid_t pid = static_cast<pid_t>(get32(req.body.data()));

    auto it = procmap.find(pid);
    if (it == procmap.end()) {
        fprintf(stderr, "[exec] wait for pid %d: not a process this server spawned\n", (int)pid);
        return false;
    }

    int status = 0;
    pid_t reaped = waitpid(pid, &status, WNOHANG);
    if (reaped == pid) {
        // Exited by now, and this call is what collected it. The same recording
        // the reaper does, so the entry reads the same either way.
        RecordExit(it->second, status);
        put32(reply.body, 1);
        put32(reply.body, static_cast<uint32_t>(status));
        return true;
    }
    if (reaped == 0) {
        // Still running. There is no status yet, so the second word is sent as 0
        // and the client goes by the state word alone.
        put32(reply.body, 0);
        put32(reply.body, 0);
        return true;
    }

    // waitpid() failed. ECHILD says the pid is no longer this process's to
    // collect, and onTick() is the only thing here that collects children, so the
    // status it recorded is the answer. It is only used when the entry agrees that
    // the child is gone: a live entry would mean the table and the kernel disagree,
    // and then the recorded status is not something that was ever observed, so it
    // must not be passed off as one.
    if (errno == ECHILD && !it->second.alive) {
        put32(reply.body, 1);
        put32(reply.body, static_cast<uint32_t>(it->second.status));
        return true;
    }
    fprintf(stderr, "[exec] waitpid(%d) failed: %s\n", (int)pid, strerror(errno));
    return false;
}

// Delivers a signal to a program this server started, on behalf of the stand-in
// that received it. The stand-in is where a terminal's signals land -- it is what
// the caller's shell left in the foreground process group -- while the program
// runs here, in the server's own process group and session, so the kernel never
// hands it the signal and no kill() aimed at the stand-in reaches it either. This
// is the other half of the catch-and-forward the client does (see the signal
// section in exec/shim.c).
//
// The pid is checked against procmap first, exactly as onWait() does and for the
// same reason: the request names it as a plain unsigned word, and without the
// lookup this would be a general-purpose kill() for any client of this socket --
// a request for pid -1 or 0 would reach the server's whole process group, and any
// child of the server could be named. procmap is the authority on which pids this
// server may be asked about, not the kernel.
bool ExecServer::onSignal(const Packet& req, Reply& reply) {
    if (req.body.size() < 8) {
        fprintf(stderr, "[exec] signal request too short: %zu bytes\n", req.body.size());
        return false;
    }
    pid_t pid = static_cast<pid_t>(get32(req.body.data()));
    int signo = static_cast<int>(get32(req.body.data() + 4));

    auto it = procmap.find(pid);
    if (it == procmap.end()) {
        fprintf(stderr, "[exec] signal %d for pid %d: not a process this server spawned\n",
                signo, (int)pid);
        return false;
    }

    // ESRCH is not a failure. The child can exit between the stand-in catching the
    // signal and this request arriving, and that race is the normal case for a
    // signal that ends a program: the status it left behind is what the next
    // OP_WAIT reports, so there is nothing left to do and nothing wrong. Every
    // other failure (EINVAL for a signal number that does not exist, EPERM) is
    // reported to the client rather than swallowed.
    if (kill(pid, signo) < 0 && errno != ESRCH) {
        fprintf(stderr, "[exec] kill(%d, %d) failed: %s\n", (int)pid, signo, strerror(errno));
        return false;
    }
    // The reply is empty: whether the signal was accepted or refused is the whole
    // answer, and the fate of the program shows up in OP_WAIT like any other end.
    return true;
}
