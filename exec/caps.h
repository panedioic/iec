// exec/caps.h — what this host can do, and the one word the probes are packed into.
//
// The four answers decide how a program is started (see ResolveKind in
// exec_server.cpp), so they are taken once at start-up and read from here after
// that. Each field is one octal digit -- three bits -- in the packed word:
//
//     fork << 9 | exec << 6 | jit << 3 | path
//
// written as four octal digits. 0o1232 therefore means fork=1, exec=2, jit=3,
// path=2; 0o1000 means fork=1 and everything else zero. (C++ spells those
// 01232 and 01000 -- it has no 0o prefix; 0o is this comment's notation, and
// the form IEC_CAPS accepts.) The packing exists so the whole answer can be
// written down as one number -- logged at start-up, overridden from the
// environment, or carried in a bug report -- without a struct having to travel
// with it.
#pragma once

#include <cstdint>

struct HostCapabilities {
    /* FORK: 0 = fork is unavailable, 1 = fork + waitpid work. */
    int fork = 0;

    /* EXEC: 0 = execve is unavailable, 1 = only the paths a sandbox keeps (the
     * one /bin/sh lives in) may be executed, 2 = the disk at large may be. */
    int exec = 0;

    /* JIT: 0 = no writable-executable memory can be obtained, 1 = RW then
     * mprotect(PROT_EXEC), 2 = memfd, 3 = anonymous RWX, 4 = prctl(jit) + RWX,
     * 5 = prctl(jit) + RX. Only "zero or not" is used for routing today; the
     * exact route is kept for diagnostics. */
    int jit = 0;

    /* PATH: 0 = only the app sandbox is reachable, 1 = part of the system path
     * is reachable, 2 = the disk at large is reachable. This is what decides
     * whether an absolute path has to be rewritten into the fake root. */
    int path = 0;
};

constexpr int kCapForkShift = 9;
constexpr int kCapExecShift = 6;
constexpr int kCapJitShift  = 3;
constexpr int kCapPathShift = 0;
constexpr uint64_t kCapMask = 0x7;

uint64_t PackCaps(const HostCapabilities& caps);
HostCapabilities UnpackCaps(uint64_t cap);

// The answers the rest of the server reads. Zero-initialised until a probe or an
// explicit set has run.
const HostCapabilities& Caps();

// Probes the host and returns the packed word. A non-empty IEC_CAPS in the
// environment takes precedence over probing. Accepts either the packed number
// in octal (0o1232, 01232 and 1232 all mean the same) or four comma-separated
// digits in the order fork,exec,jit,path.
uint64_t ProbeCapabilities();

// Compatibility with the earlier signature: probe and write the word out.
void ProbeCapabilities(uint64_t* cap);

// Pins the answers explicitly. This is what main.cpp uses to make the routing of
// a build reproducible. There is no "already probed" flag beside it, so a later
// call to ProbeCapabilities() would overwrite this value.
void setCaps(uint64_t cap);
