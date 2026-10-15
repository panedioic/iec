// common/sock_operation.h - wire operation numbers.
//
// Shared by the C shim (tty/shim.c) and the C++ server (tty/tty_server.cpp) so the
// two cannot drift apart. The values are part of the wire protocol: keep them
// explicit and only ever append.
//
// A plain enum, not "enum : uint32_t", so that this header also compiles as C.
#ifndef SOCK_OPERATION_H
#define SOCK_OPERATION_H

enum Op {
    OP_PING     = 0,
    OP_PONG     = 1,
    // Reserved. The shim implements forkpty() as openpty() + fork() + login_tty()
    // locally, so the server is never asked to fork.
    OP_FORKPTY  = 2,
    // Reserved. The fds of the faked pty live in the client process, so there is
    // nothing for the server to ioctl; the shim emulates the tty ioctls locally.
    OP_IOCTL    = 3,
    OP_OPENPTMX = 4,   /* open("/dev/ptmx")   -> master fd + pts number */
    OP_OPENPTS  = 5,   /* open("/dev/pts/<n>") -> slave fd */
};

#endif /* SOCK_OPERATION_H */
