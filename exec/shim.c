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
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The exec service listens on its own socket, not the tty service's: a client
 * reaches a service by connecting to it, so the two paths must differ. */
#define EXEC_SOCK_PATH "/tmp/exec_service.sock"

/* ------------------------------------------------------------------- */
/* Server round trip                                                   */
/*                                                                     */
/* The round trip lives in common/rpc.c, shared with tty/shim.c; these */
/* are thin bindings to it, so the call sites below stay readable.     */
/* Only what this shim actually uses is bound here -- nothing takes an */
/* fd back or expects a fixed-size reply yet -- because a wrapper that */
/* nothing calls is exactly the unused-function warning this file had. */
/* ------------------------------------------------------------------- */

static void body_put32(uint8_t *p, uint32_t v) { iec_put32(p, v); }

static int rpc_call(uint32_t op, const void *body, uint32_t len,
                    void *out, uint32_t out_max, uint32_t *out_len) {
    return iec_rpc_call(EXEC_SOCK_PATH, op, body, len, out, out_max, out_len);
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
     *   then NUL-terminated strings, in this order:
     *     file, cwd (empty means inherit)
     *     argc entries for argv, then envc entries for env
     *
     * Any fds sent with the request (SCM_RIGHTS) become the child's stdin,
     * stdout, stderr, ... in the order they are attached.
     */
    enum { EXEC_REQ_MAX = 8192, EXEC_FLAG_WAIT = 1 };
    uint8_t req[EXEC_REQ_MAX];
    size_t off = 12, argc = 0, envc = 0, i, len;

    if (!file) {
        errno = ENOENT;
        return -1;
    }
    while (argv && argv[argc]) argc++;
    while (envp && envp[envc]) envc++;

    body_put32(req, EXEC_FLAG_WAIT);
    body_put32(req + 4, (uint32_t)argc);
    body_put32(req + 8, (uint32_t)envc);

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

    if (rpc_call(OP_EXECUTE, req, (uint32_t)off, NULL, 0, NULL) < 0)
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
