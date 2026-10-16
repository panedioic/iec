// exec/exec_types.h — what a request to start a program carries, and the ways one
// can be started.
//
// Both halves of the exec service need these: exec_server.cpp builds the requests
// and decides how to serve them, and the launcher for a shared object (exec_so.cpp)
// reads one to run its entry. They live here so that the launcher can see the shape
// without including the server.
#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

#include "sock_operation.h"

// How a program is started. ResolveKind() in exec_server.cpp picks one from the host
// capabilities probed there; StartByKind() in the same file is what carries it out.
enum class LaunchKind {
    kAuto,          // nobody has decided yet
    kExec,          // plain fork + execve
    kSo,            // a shared object, entered through the loader
    kLoader,        // a loader is executed in the program's place
    kQemu_x86_64,   // emulated, qemu in the program's place
    kQemu_aarch64,
    kShebang,
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
    CallMethod  call_method = CALL_METHOD_END;
    pid_t       caller_pid = 0;
};
