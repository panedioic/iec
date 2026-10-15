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
    // Reserved. The tty state lives on the server now, so this op is unused.
    OP_IOCTL    = 3,
    OP_OPENPTMX = 4,   /* open("/dev/ptmx")   -> master fd + pts number */
    OP_OPENPTS  = 5,   /* open("/dev/pts/<n>") -> slave fd */

    // The server owns the termios, the window size and the foreground process
    // group of every pty, so the client asks for them instead of keeping a copy
    // that would drift. Every request names its pts in the first 4 body bytes,
    // because the client's fd numbers mean nothing on the server.
    //
    // termios and winsize travel as plain struct bytes: both processes run on
    // the same machine and the same ABI, and the struct is a fixed-size value
    // type with no padding traps.
    OP_TCGETATTR = 6,  /* [pts]              -> struct termios */
    OP_TCSETATTR = 7,  /* [pts][action][termios] */
    OP_TCGETPGRP = 8,  /* [pts]              -> pid */
    OP_TCSETPGRP = 9,  /* [pts][pid] */
    OP_GETWINSZ  = 10, /* [pts]              -> struct winsize */
    OP_SETWINSZ  = 11, /* [pts][winsize] */
};

#endif /* SOCK_OPERATION_H */
