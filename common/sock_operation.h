// common/sock_operation.h - wire operation numbers.
//
// Shared by the C shims and the C++ servers so the two cannot drift apart. The
// values are part of the wire protocol: keep them explicit and only ever append.
//
// Each service is reached over its own socket, so numbers only have to be unique
// within one service -- except for OpCommon, which every service answers with the
// same number. That shared block is also why the per-service enums start at 2:
// enum members live in the surrounding scope, so two enums that both defined
// OP_PING would collide at compile time rather than politely coexist.
//
// Plain enums, not "enum : uint32_t", so that this header also compiles as C.
#ifndef SOCK_OPERATION_H
#define SOCK_OPERATION_H

#include <assert.h>

#define OP_COMMON_START 1
#define OP_TTY_START    101
#define OP_EXEC_START   201

/* Answered by every service, on every socket. */
enum OpCommon {
    OP_PING = OP_COMMON_START,
    OP_PONG = OP_COMMON_START + 1,

    OP_COMMON_END,
};

enum OpTty {
    // Reserved. The shim implements forkpty() as openpty() + fork() + login_tty()
    // locally, so the server is never asked to fork.
    OP_FORKPTY  = OP_TTY_START,
    // Reserved. The tty state lives on the server now, so this op is unused.
    OP_IOCTL    = OP_TTY_START + 1,
    OP_OPENPTMX = OP_TTY_START + 2,   /* open("/dev/ptmx")   -> master fd + pts number */
    OP_OPENPTS  = OP_TTY_START + 3,   /* open("/dev/pts/<n>") -> slave fd */

    // The server owns the termios, the window size and the foreground process
    // group of every pty, so the client asks for them instead of keeping a copy
    // that would drift. Every request names its pts in the first 4 body bytes,
    // because the client's fd numbers mean nothing on the server.
    //
    // termios and winsize travel as plain struct bytes: both processes run on
    // the same machine and the same ABI, and the struct is a fixed-size value
    // type with no padding traps.
    OP_TCGETATTR = OP_TTY_START + 4,  /* [pts]              -> struct termios */
    OP_TCSETATTR = OP_TTY_START + 5,  /* [pts][action][termios] */
    OP_TCGETPGRP = OP_TTY_START + 6,  /* [pts]              -> pid */
    OP_TCSETPGRP = OP_TTY_START + 7,  /* [pts][pid] */
    OP_GETWINSZ  = OP_TTY_START + 8,  /* [pts]              -> struct winsize */
    OP_SETWINSZ  = OP_TTY_START + 9,  /* [pts][winsize] */

    OP_TTY_END,
};

enum OpExec {
    // Layout documented next to the encoder in exec/shim.c.
    OP_EXECUTE = OP_EXEC_START,
    // [4 bytes remote pid] -> [4 bytes state][4 bytes status]; see the wait loop
    // in exec/shim.c for what the client does with the two answer words.
    OP_WAIT    = OP_EXEC_START + 1,

    OP_EXEC_END,
};

/* Compile-time guard that the numbering cannot collide. This header is included
 * from C too, where the spelling is _Static_assert (a C11 keyword) and the C++
 * static_assert is not a keyword at all. */
#ifdef __cplusplus
static_assert(OP_COMMON_END <= OP_TTY_START, "OpTty reuses an OpCommon number");
static_assert(OP_TTY_END <= OP_EXEC_START, "OpExec reuses an OpTty number");
#else
_Static_assert(OP_COMMON_END <= OP_TTY_START, "OpTty reuses an OpCommon number");
_Static_assert(OP_TTY_END <= OP_EXEC_START, "OpExec reuses an OpTty number");
#endif

#endif /* SOCK_OPERATION_H */
