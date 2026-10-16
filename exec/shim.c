// exec/shim.c — interception of the exec family
//
// The same idea as the pty hooks next door: the sandboxed client hands the call
// to the out-of-process server instead of doing it itself. For now this only
// reports that it was reached, and fails, so the interception can be observed
// before anything is built on top of it.

#include "intercept.h"
#include "rpc.h"
#include "sock_operation.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* The exec service listens on its own socket, not the tty service's: a client
 * reaches a service by connecting to it, so the two paths must differ. */
#define EXEC_SOCK_PATH "/tmp/exec_service.sock"

/* ----------------------------------------------------------------------- */
/* Server round trip                                                       */
/*                                                                         */
/* The round trip lives in common/rpc.c, shared with tty/shim.c; this is   */
/* the thin binding to it, so the call site below stays readable. Only     */
/* what this shim actually uses is bound: the request carries the caller's */
/* stdio to the server, and nothing here takes an fd back or expects a     */
/* fixed-size reply, so the plain-reply and fd-reply variants are not      */
/* bound either -- a wrapper that nothing calls is exactly the             */
/* unused-function warning this file had.                                  */
/* ----------------------------------------------------------------------- */

static void body_put32(uint8_t *p, uint32_t v) { iec_put32(p, v); }

/* OP_EXECUTE is the one request in this program that sends fds, so this is the
 * only variant bound here; it answers with nothing. */
static int rpc_call_fds(uint32_t op, const void *body, uint32_t len,
                        const int *fds, int nfds) {
    return iec_rpc_call_fds(EXEC_SOCK_PATH, op, body, len, fds, nfds, NULL, 0, NULL);
}

/* Collects a NULL-terminated varargs argument list into argv, which must have
 * room for max entries including the terminator. */
static void va_to_argv(const char *arg0, va_list ap, char *argv[], int max) {
    int i = 0;

    if (arg0 && i < max - 1) argv[i++] = (char *)arg0;
    for (;;) {
        char *arg = va_arg(ap, char *);
        if (!arg) break;
        if (i < max - 1) argv[i++] = arg;
    }
    argv[i] = NULL;
}

static int exec_common(const char *file, char *const argv[], char *const envp[]) {
    /* Compact request for OP_EXECUTE. Everything is length-delimited by the
     * packet itself plus NUL terminators, and the decoder checks the buffer end
     * as it walks, so a truncated or hostile request cannot run off it:
     *
     *   [0..3]   flags      bit0 = wait for the child before replying
     *                       bit1 = hand the child our stdio
     *   [4..7]   argc
     *   [8..11]  envc
     *   [12..15] fd_count
     *   fd_count x [4 bytes target_fd]
     *   then NUL-terminated strings, in this order:
     *     file, cwd (empty means inherit)
     *     argc entries for argv, then envc entries for env
     *
     * The fds sent with the request (SCM_RIGHTS) are not named in the body; the
     * target list says, position by position, which fd each of them has to
     * become in the child. The list is not redundant: a descriptor that is
     * closed here -- a shell started with 0, 1 or 2 missing -- is left out
     * entirely, and then counting along the received fds would map stderr onto
     * stdout.
     */
    enum { EXEC_REQ_MAX = 8192, EXEC_FLAG_WAIT = 1, EXEC_FLAG_STDIO = 2, EXEC_STDIO_FDS = 3 };
    uint8_t req[EXEC_REQ_MAX];
    int send_fds[EXEC_STDIO_FDS];
    uint32_t targets[EXEC_STDIO_FDS];
    int nfds = 0, fd;
    size_t off, argc = 0, envc = 0, i, len;

    if (!file) {
        errno = ENOENT;
        return -1;
    }
    while (argv && argv[argc]) argc++;
    while (envp && envp[envc]) envc++;

    /* The child is started on the server, so our own stdio -- the terminal this
     * exec was called from -- has to travel with the request; without it the
     * child writes to whatever the server was started with. A closed fd is
     * skipped, which is what the target list is for. */
    for (fd = STDIN_FILENO; fd <= STDERR_FILENO; fd++) {
        if (fcntl(fd, F_GETFD) < 0) continue;
        send_fds[nfds] = fd;
        targets[nfds] = (uint32_t)fd;
        nfds++;
    }

    /* The strings start after the header and the target list, so the offset of
     * every string below moves with fd_count. */
    off = 16 + 4 * (size_t)nfds;
    body_put32(req, EXEC_FLAG_WAIT | (nfds > 0 ? EXEC_FLAG_STDIO : 0u));
    body_put32(req + 4, (uint32_t)argc);
    body_put32(req + 8, (uint32_t)envc);
    body_put32(req + 12, (uint32_t)nfds);
    for (i = 0; i < (size_t)nfds; i++) body_put32(req + 16 + 4 * i, targets[i]);

#define PUT_CSTR(s)                                     \
    do {                                                \
        len = strlen(s) + 1;                            \
        if (off + len > EXEC_REQ_MAX) goto too_long;    \
        memcpy(req + off, (s), len);                    \
        off += len;                                     \
    } while (0)

    PUT_CSTR(file);
    PUT_CSTR("");                          /* cwd: inherit */
    for (i = 0; i < argc; i++) PUT_CSTR(argv[i]);
    for (i = 0; i < envc; i++) PUT_CSTR(envp[i]);
#undef PUT_CSTR

    if (rpc_call_fds(OP_EXECUTE, req, (uint32_t)off, send_fds, nfds) < 0)
        return -1;

    /* A real exec never returns: it replaces the process image. Until the child
     * is actually forked and handed the fds on the other side, report failure so
     * the caller does not carry on as if it had been replaced. */
    errno = EACCES;
    return -1;

too_long:
    fprintf(stderr, "[shim] exec request exceeds %d bytes\n", EXEC_REQ_MAX);
    errno = E2BIG;
    return -1;
    
    errno = EACCES;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Hooks                                                              */
/* ------------------------------------------------------------------ */

int IEC_EXPORT(execve)(const char *path, char *const argv[], char *const envp[]) {
    return exec_common(path, argv, envp);
}

int IEC_EXPORT(execv)(const char *path, char *const argv[]) {
    return exec_common(path, argv, NULL);
}

int IEC_EXPORT(execvp)(const char *file, char *const argv[]) {
    return exec_common(file, argv, NULL);
}

int IEC_EXPORT(execvpe)(const char *file, char *const argv[], char *const envp[]) {
    return exec_common(file, argv, envp);
}

int IEC_EXPORT(execl)(const char *path, const char *arg0, ...) {
    char *argv[64];
    va_list ap;

    va_start(ap, arg0);
    va_to_argv(arg0, ap, argv, 64);
    va_end(ap);
    return exec_common(path, argv, NULL);
}

int IEC_EXPORT(execlp)(const char *file, const char *arg0, ...) {
    char *argv[64];
    va_list ap;

    va_start(ap, arg0);
    va_to_argv(arg0, ap, argv, 64);
    va_end(ap);
    return exec_common(file, argv, NULL);
}

/* execle is the odd one out: the environment follows the terminating NULL of the
 * argument list, so the varargs have to be walked before it can be read. */
int IEC_EXPORT(execle)(const char *path, const char *arg0, ...) {
    char *argv[64];
    char *const *envp;
    va_list ap;

    va_start(ap, arg0);
    va_to_argv(arg0, ap, argv, 64);
    envp = va_arg(ap, char *const *);
    va_end(ap);
    return exec_common(path, argv, envp);
}
