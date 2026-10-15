// tty/shim.c — client-side interception of the pty API
//
// A sandboxed client cannot open /dev/ptmx, so every entry point that would
// touch a real pty is rerouted to the out-of-process tty server over a Unix
// domain socket. The server answers with real file descriptors (passed back with
// SCM_RIGHTS), and those are returned to the caller as if it had opened them
// locally. The fds are ordinary socketpair ends, so read()/write() work on them
// with no further interception.
//
// What the client gets is not a tty, though, so the tty-flavoured calls
// (isatty, tcgetattr/tcsetattr, the tty ioctls) are emulated here against a small
// per-fd table. Everything that is not pty-related falls through to the real
// libc function.

#include "intercept.h"
#include "sock_client.h"
#include "sock_operation.h"

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <utmp.h>

#define TTY_SOCK_PATH "/tmp/tty_service.sock"
#define PTMX_PATH     "/dev/ptmx"
#define PTS_PREFIX    "/dev/pts/"
#define PTS_PREFIX_LEN (sizeof PTS_PREFIX - 1)
#define SHIM_MAX_FD   256

#ifdef INT_WARP
/* -Wl,--wrap=<name> has the linker provide the original function under
 * __real_<name>, but the compiler still needs a declaration to reference it. */
extern int __real_open(const char *, int, ...);
extern int __real_close(int);
extern ssize_t __real_read(int, void *, size_t);
extern int __real_dup(int);
extern int __real_dup2(int, int);
extern int __real_dup3(int, int, int);
extern int __real_fcntl(int, int, ...);
extern int __real_ioctl(int, unsigned long, ...);
extern int __real_isatty(int);
extern int __real_tcgetattr(int, struct termios *);
extern int __real_tcsetattr(int, int, const struct termios *);
extern pid_t __real_tcgetpgrp(int);
extern int __real_tcsetpgrp(int, pid_t);
extern int __real_grantpt(int);
extern int __real_unlockpt(int);
extern char *__real_ptsname(int);
extern char *__real_ttyname(int);
extern int __real_ttyname_r(int, char *, size_t);
#endif

/* Resolves the real libc function, once. Under INT_PRELOAD this is a
 * dlsym(RTLD_NEXT); under INT_WARP it is the linker-provided __real_<name>.
 * IEC_REAL already performs the assignment, so use this as a plain statement:
 * writing "ptr = IEC_REAL(ptr, name)" would assign ptr twice without a sequence
 * point between them. */
#define RESOLVE(ptr, name) IEC_REAL(ptr, name)

static int (*real_open)(const char *, int, ...);
static int (*real_close)(int);
static ssize_t (*real_read)(int, void *, size_t);
static int (*real_dup)(int);
static int (*real_dup2)(int, int);
static int (*real_dup3)(int, int, int);
static int (*real_fcntl)(int, int, ...);
static int (*real_ioctl)(int, unsigned long, ...);
static int (*real_isatty)(int);
static int (*real_tcgetattr)(int, struct termios *);
static int (*real_tcsetattr)(int, int, const struct termios *);
static pid_t (*real_tcgetpgrp)(int);
static int (*real_tcsetpgrp)(int, pid_t);
static int (*real_grantpt)(int);
static int (*real_unlockpt)(int);
static char *(*real_ptsname)(int);
static char *(*real_ttyname)(int);
static int (*real_ttyname_r)(int, char *, size_t);

#define IEC_TTY_ENV   "IEC_TTY_PTS"
#define SHIM_MAX_PTS  64

/* ------------------------------------------------------------------ */
/* Emulated tty state                                                  */
/*                                                                     */
/* The master and every slave fd of one pts share a single state block, */
/* the way one line discipline backs both ends of a real pty: setting   */
/* the window size on the master has to be visible through the slave.   */
/* ------------------------------------------------------------------ */

struct shim_tty {
    int used;
    int pts_n;              /* the "/dev/pts/<n>" number */
    pid_t fg_pgrp;          /* emulated foreground process group (job control) */
    struct termios tio;
    struct winsize ws;
};

static struct shim_tty g_pts[SHIM_MAX_PTS];

struct shim_fd {
    int used;
    int is_master;
    struct shim_tty *st;
};

static struct shim_fd g_fd[SHIM_MAX_FD];

/* Returns the fd's state block, or NULL if this fd is not one of ours. */
static struct shim_tty *tty_of(int fd) {
    if (fd < 0 || fd >= SHIM_MAX_FD || !g_fd[fd].used) return NULL;
    return g_fd[fd].st;
}

/* Returns the state block of an already known pts, without creating one. */
static struct shim_tty *state_find(int pts_n) {
    int i;
    for (i = 0; i < SHIM_MAX_PTS; i++)
        if (g_pts[i].used && g_pts[i].pts_n == pts_n) return &g_pts[i];
    return NULL;
}

/* Finds an fd in this process that is already bound to the given pts, so that
 * open("/dev/pts/<n>") can be answered with a dup instead of a round trip. A
 * shell reopens its terminal by name in every process it spawns, and the server
 * has long since given its copy of the slave away. */
static int fd_for_pts(struct shim_tty *st, int want_master) {
    int fd;
    for (fd = 0; fd < SHIM_MAX_FD; fd++) {
        if (!g_fd[fd].used || g_fd[fd].st != st) continue;
        if (g_fd[fd].is_master == want_master) return fd;
    }
    return -1;
}

/* Finds or creates the state block for a pts number. */
static struct shim_tty *state_get(int pts_n) {
    struct shim_tty *s = NULL;
    int i;

    for (i = 0; i < SHIM_MAX_PTS; i++)
        if (g_pts[i].used && g_pts[i].pts_n == pts_n) return &g_pts[i];
    for (i = 0; i < SHIM_MAX_PTS; i++)
        if (!g_pts[i].used) { s = &g_pts[i]; break; }
    if (!s) return NULL;

    memset(s, 0, sizeof *s);
    s->used = 1;
    s->pts_n = pts_n;
    /* A foreground group of 0 means "none", which makes a shell think another
     * process owns the terminal; start out owning it ourselves. */
    s->fg_pgrp = getpgrp();
    /* A plain cooked terminal, i.e. what openpty() would leave behind. */
    s->tio.c_iflag = ICRNL | IXON;
    s->tio.c_oflag = OPOST | ONLCR;
    s->tio.c_cflag = CS8 | CREAD | CLOCAL;
    s->tio.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN;
    s->tio.c_cc[VMIN] = 1;
    s->tio.c_cc[VTIME] = 0;
    /* The control characters matter as much as the flags here: the struct was
     * memset, so without these VERASE would be NUL and backspace would be
     * treated as an erase request for every NUL byte typed. */
    s->tio.c_cc[VINTR] = 0x03;    /* ^C  */
    s->tio.c_cc[VQUIT] = 0x1c;    /* ^\  */
    s->tio.c_cc[VERASE] = 0x7f;   /* DEL */
    s->tio.c_cc[VKILL] = 0x15;    /* ^U  */
    s->tio.c_cc[VEOF] = 0x04;     /* ^D  */
    s->tio.c_cc[VSUSP] = 0x1a;    /* ^Z  */
    s->ws.ws_row = 24;
    s->ws.ws_col = 80;
    return s;
}

static void tty_set(int fd, int pts_n, int is_master) {
    struct shim_tty *s = state_get(pts_n);
    if (fd < 0 || fd >= SHIM_MAX_FD || !s) return;
    g_fd[fd].used = 1;
    g_fd[fd].is_master = is_master;
    g_fd[fd].st = s;
}

static void tty_clear(int fd) {
    if (fd >= 0 && fd < SHIM_MAX_FD) memset(&g_fd[fd], 0, sizeof g_fd[fd]);
}

/* dup2() makes a second name for the same tty; keep the table in sync so that
 * isatty(0)/tcgetattr(0) work in the shell child after login_tty(). */
static void tty_copy(int from, int to) {
    if (from < 0 || from >= SHIM_MAX_FD || to < 0 || to >= SHIM_MAX_FD) return;
    if (!g_fd[from].used) return;
    g_fd[to] = g_fd[from];
}

/* forkpty()'s child almost always execs something else, and this table does not
 * survive an exec. login_tty() records the pts number in the environment, and
 * this constructor rebuilds the mapping for fds 0..2 in the new image, so that
 * the shell still sees a tty. */
__attribute__((constructor))
static void shim_resume_from_exec(void) {
    const char *env = getenv(IEC_TTY_ENV);
    struct shim_tty *s;
    int fd;

    if (!env || *env < '0' || *env > '9') return;
    s = state_get(atoi(env));
    if (!s) return;
    for (fd = STDIN_FILENO; fd <= STDERR_FILENO; fd++) {
        if (fcntl(fd, F_GETFD) < 0) continue;
        g_fd[fd].used = 1;
        g_fd[fd].is_master = 0;
        g_fd[fd].st = s;
    }
}

/* ------------------------------------------------------------------ */
/* Server round trip                                                   */
/* ------------------------------------------------------------------ */

static uint32_t body_get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void body_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Sends one request and returns the fd the server sent back (-1 on failure).
 * If out_word is not NULL it receives the first 4 body bytes as a big-endian
 * word (the pts number for OP_OPENPTMX). */
static int rpc_take_fd(uint32_t op, const void *body, uint32_t len, uint32_t *out_word) {
    sock_client c;
    sock_reply r;
    int rc, fd;

    if (sock_connect(&c, TTY_SOCK_PATH) < 0) {
        fprintf(stderr, "[shim] connect %s failed: %s\n", TTY_SOCK_PATH, strerror(errno));
        errno = EIO;
        return -1;
    }

    rc = sock_call(&c, op, body, len, &r);
    if (rc < 0) {
        fprintf(stderr, "[shim] op %u failed: %s\n", (unsigned)op, strerror(errno));
        sock_close(&c);
        errno = EIO;
        return -1;
    }
    if (rc > 0) {
        fprintf(stderr, "[shim] op %u: server error: %s\n", (unsigned)op,
                r.body ? (char *)r.body : "(no message)");
        sock_reply_free(&r);
        sock_close(&c);
        errno = EIO;
        return -1;
    }
    if (r.nfds < 1) {
        fprintf(stderr, "[shim] op %u: reply carried no fd\n", (unsigned)op);
        sock_reply_free(&r);
        sock_close(&c);
        errno = EIO;
        return -1;
    }

    if (out_word && r.body && r.len >= 4) *out_word = body_get32(r.body);
    /* Take ownership: the reply must not close the fd we are about to return. */
    fd = sock_reply_take_fd(&r, 0);
    sock_reply_free(&r);
    sock_close(&c);
    return fd;
}

/* ------------------------------------------------------------------ */
/* Hooks                                                               */
/* ------------------------------------------------------------------ */

int IEC_EXPORT(open)(const char *path, int flags, ...) {
    uint32_t pts_n = 0;
    int fd;

    /* open() is declared __nonnull, so path is never NULL here. */
    if (strcmp(path, PTMX_PATH) == 0) {
        fd = rpc_take_fd(OP_OPENPTMX, NULL, 0, &pts_n);
        if (fd < 0) return -1;
        tty_set(fd, (int)pts_n, 1);
        return fd;
    }
    if (strncmp(path, PTS_PREFIX, PTS_PREFIX_LEN) == 0) {
        const char *num = path + PTS_PREFIX_LEN;
        uint8_t req[4];
        struct shim_tty *st;
        int n, local;

        if (*num < '0' || *num > '9') {
            errno = ENODEV;
            return -1;
        }
        n = atoi(num);

        /* If this process already holds the slave end, dup it: the server gave
         * its own copy away on the first open and will not have another. */
        st = state_find(n);
        if (st) {
            local = fd_for_pts(st, 0);
            if (local >= 0) {
                fd = dup(local);
                if (fd >= 0) {
                    tty_set(fd, n, 0);
                    return fd;
                }
                return -1;
            }
        }

        body_put32(req, (uint32_t)n);
        fd = rpc_take_fd(OP_OPENPTS, req, sizeof req, NULL);
        if (fd < 0) return -1;
        tty_set(fd, n, 0);
        return fd;
    }

    /* Not a pty path: let the real open() have it. */
    RESOLVE(real_open, open);
    if (flags & O_CREAT) {
        va_list ap;
        mode_t mode;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
        return real_open(path, flags, mode);
    }
    return real_open(path, flags);
}

int IEC_EXPORT(close)(int fd) {
    tty_clear(fd);
    RESOLVE(real_close, close);
    return real_close(fd);
}

/* The line discipline.
 *
 * A real pty runs two filters around the fd pair. Output: OPOST|ONLCR turns each
 * "\n" a program writes into "\r\n" on its way to the terminal. Input: control
 * characters become signals, CR folds to NL, canonical mode assembles whole
 * lines, and what was typed is echoed back.
 *
 * Neither happens over a plain socketpair, so both are reproduced here. The
 * output half runs on the master's read path, the one place both injection modes
 * reach it (the shell in warp mode is not wrapped). The input half has to run on
 * the slave's read path instead: only the process doing the read holds the live
 * termios -- readline edits its own copy -- and knows which process group should
 * get the signals.
 */

#define LINE_MAX 4096

/* Canonical-mode input being assembled, one buffer per pts. */
static unsigned char g_line[SHIM_MAX_PTS][LINE_MAX];
static size_t g_line_len[SHIM_MAX_PTS];
static int g_line_ready[SHIM_MAX_PTS];

static int state_index(const struct shim_tty *t) {
    return (int)(t - g_pts);
}

static void line_put(int idx, unsigned char c) {
    if (g_line_len[idx] < LINE_MAX) {
        g_line[idx][g_line_len[idx]++] = c;
    } else {
        /* Line limit reached: release what we have so the reader can drain it
         * instead of deadlocking on a line that can never complete. */
        g_line_ready[idx] = 1;
    }
}

/* Writes a typed character back out through the slave fd, i.e. to the master, so
 * the emulator draws it. Best effort: there is nothing useful to do if the
 * terminal has gone away. */
static void input_echo(struct shim_tty *t, int fd, unsigned char c) {
    if ((t->tio.c_lflag & ECHOCTL) && c < 0x20 && c != '\t' && c != '\n') {
        unsigned char out[2];
        out[0] = '^';
        out[1] = (unsigned char)(c + '@');
        if (write(fd, out, 2) != 2) return;
        return;
    }
    if (write(fd, &c, 1) != 1) return;
}

static void input_erase_echo(int fd) {
    if (write(fd, "\b \b", 3) != 3) return;
}

static void input_filter(struct shim_tty *t, int fd, const unsigned char *in, size_t len) {
    int idx = state_index(t);
    int canon = (t->tio.c_lflag & ICANON) != 0;
    int echo = (t->tio.c_lflag & ECHO) != 0;
    size_t i;

    for (i = 0; i < len; i++) {
        unsigned char c = in[i];

        if ((t->tio.c_iflag & ICRNL) && c == '\r') c = '\n';

        if ((t->tio.c_lflag & ISIG) &&
            (c == t->tio.c_cc[VINTR] || c == t->tio.c_cc[VQUIT] ||
             c == t->tio.c_cc[VSUSP])) {
            int sig = c == t->tio.c_cc[VINTR] ? SIGINT
                    : c == t->tio.c_cc[VQUIT] ? SIGQUIT
                                              : SIGTSTP;
            if (echo) input_echo(t, fd, c);
            /* Signal the foreground group, but only inside our own session. A
             * stale or bogus group number must never reach unrelated processes:
             * the terminal, the shell that started it, or this agent's own
             * shell are all one bad kill(-n) away. */
            if (t->fg_pgrp > 0 && getsid(t->fg_pgrp) == getsid(0))
                kill(-t->fg_pgrp, sig);
            g_line_len[idx] = 0;   /* the partly typed line is discarded */
            continue;
        }

        if (canon && t->tio.c_cc[VERASE] != _POSIX_VDISABLE &&
            c == t->tio.c_cc[VERASE]) {
            if (g_line_len[idx] > 0) {
                g_line_len[idx]--;
                if (echo && (t->tio.c_lflag & ECHOE)) input_erase_echo(fd);
            }
            continue;
        }

        line_put(idx, c);
        if (echo || (canon && (t->tio.c_lflag & ECHONL) && c == '\n'))
            input_echo(t, fd, c);

        if (!canon || c == '\n') g_line_ready[idx] = 1;
    }
}

ssize_t IEC_EXPORT(read)(int fd, void *buf, size_t n) {
    struct shim_tty *t = tty_of(fd);
    unsigned char *b = buf;
    size_t room, i, o, produced;
    ssize_t r;

    RESOLVE(real_read, read);

    if (!t) return real_read(fd, buf, n);

    /* is_master is per fd, not per pts: the state block is shared by both ends,
     * and only the fd opened through /dev/ptmx carries the master's output. */
    if (g_fd[fd].is_master) {
        /* Output half. The raw bytes go into the second half of the caller's
         * buffer and the expanded result into the first, so the two never
         * overlap; reading at most half the buffer guarantees every newline has
         * room to double. That is what lets this work without a pending buffer
         * -- holding bytes back would make poll()/epoll() stop reporting the fd
         * as readable while data was still owed to the caller. */
        if (n < 2 || !(t->tio.c_oflag & OPOST) || !(t->tio.c_oflag & ONLCR))
            return real_read(fd, buf, n);

        room = n / 2;
        r = real_read(fd, b + room, room);
        if (r <= 0) return r;

        for (i = 0, o = 0; i < (size_t)r; i++) {
            if (b[room + i] == '\n') b[o++] = '\r';
            b[o++] = b[room + i];
        }
        return (ssize_t)o;
    }

    /* Input half. Blocking here while a line is still incomplete is correct: a
     * canonical read() is supposed to wait for one. */
    for (;;) {
        int idx = state_index(t);

        if (g_line_ready[idx]) {
            produced = 0;
            while (g_line_ready[idx] && produced < n) {
                size_t take = g_line_len[idx];
                if (take > n - produced) take = n - produced;
                memcpy(b + produced, g_line[idx], take);
                memmove(g_line[idx], g_line[idx] + take, g_line_len[idx] - take);
                g_line_len[idx] -= take;
                produced += take;
                if (g_line_len[idx] == 0) g_line_ready[idx] = 0;
            }
            return (ssize_t)produced;
        }

        {
            unsigned char tmp[512];
            r = real_read(fd, tmp, sizeof tmp);
            if (r < 0) return -1;
            if (r == 0) return 0;
            input_filter(t, fd, tmp, (size_t)r);
        }
    }
}

/* Duplicating a tty fd has to carry the table entry over, otherwise the copy is
 * an anonymous socket again. This is not a corner case: bash keeps its terminal
 * in fd 255 with dup2(dup(stderr), 255) while setting up job control, and every
 * later tcgetpgrp()/tcsetpgrp()/isatty() on that fd would otherwise fail with
 * ENOTTY. */
int IEC_EXPORT(dup)(int oldfd) {
    int fd;
    RESOLVE(real_dup, dup);
    fd = real_dup(oldfd);
    if (fd >= 0) tty_copy(oldfd, fd);
    return fd;
}

int IEC_EXPORT(dup2)(int oldfd, int newfd) {
    int fd;
    RESOLVE(real_dup2, dup2);
    fd = real_dup2(oldfd, newfd);
    if (fd >= 0) {
        tty_clear(fd);
        tty_copy(oldfd, fd);
    }
    return fd;
}

int IEC_EXPORT(dup3)(int oldfd, int newfd, int flags) {
    int fd;
    RESOLVE(real_dup3, dup3);
    fd = real_dup3(oldfd, newfd, flags);
    if (fd >= 0) {
        tty_clear(fd);
        tty_copy(oldfd, fd);
    }
    return fd;
}

int IEC_EXPORT(fcntl)(int fd, int cmd, ...) {
    va_list ap;
    void *arg;
    int r;

    va_start(ap, cmd);
    arg = va_arg(ap, void *);
    va_end(ap);

    RESOLVE(real_fcntl, fcntl);
    r = real_fcntl(fd, cmd, arg);

    /* fcntl(F_DUPFD) is the other way a program makes a second name for a fd. */
    if (r >= 0 && (cmd == F_DUPFD
#ifdef F_DUPFD_CLOEXEC
                   || cmd == F_DUPFD_CLOEXEC
#endif
                   ))
        tty_copy(fd, r);
    return r;
}

int IEC_EXPORT(isatty)(int fd) {
    if (tty_of(fd)) return 1;
    RESOLVE(real_isatty, isatty);
    return real_isatty(fd);
}

/* ttyname()/ttyname_r() also have to be hooked on their own: glibc implements
 * them on top of the internal __isatty (which issues __ioctl(TCGETS) itself), so
 * with only isatty()/ioctl() hooked they still report ENOTTY. A shell asks for
 * the terminal name while setting up job control and disables job control when
 * it cannot get one ("cannot set terminal process group"). */
char *IEC_EXPORT(ttyname)(int fd) {
    static char buf[32];
    struct shim_tty *t = tty_of(fd);

    if (!t) {
        RESOLVE(real_ttyname, ttyname);
        return real_ttyname(fd);
    }
    snprintf(buf, sizeof buf, "/dev/pts/%d", t->pts_n);
    return buf;
}

int IEC_EXPORT(ttyname_r)(int fd, char *buf, size_t buflen) {
    struct shim_tty *t = tty_of(fd);
    int n;

    if (!t) {
        RESOLVE(real_ttyname_r, ttyname_r);
        return real_ttyname_r(fd, buf, buflen);
    }
    n = snprintf(buf, buflen, "/dev/pts/%d", t->pts_n);
    if (n < 0) return errno;
    if ((size_t)n >= buflen) return ERANGE;
    return 0;
}

int IEC_EXPORT(tcgetattr)(int fd, struct termios *tio) {
    struct shim_tty *t = tty_of(fd);
    if (t) {
        if (!tio) { errno = EFAULT; return -1; }
        *tio = t->tio;
        return 0;
    }
    RESOLVE(real_tcgetattr, tcgetattr);
    return real_tcgetattr(fd, tio);
}

int IEC_EXPORT(tcsetattr)(int fd, int actions, const struct termios *tio) {
    struct shim_tty *t = tty_of(fd);
    if (t) {
        if (!tio) { errno = EFAULT; return -1; }
        t->tio = *tio;
        return 0;
    }
    RESOLVE(real_tcsetattr, tcsetattr);
    return real_tcsetattr(fd, actions, tio);
}

/* tcgetpgrp()/tcsetpgrp() have to be hooked on their own: glibc implements them
 * with libc's internal __ioctl, which LD_PRELOAD cannot interpose, so the ioctl
 * hook below never sees these calls. A shell uses them to take the foreground,
 * and when they fail it prints "cannot set terminal process group" and gives up
 * job control entirely. */
pid_t IEC_EXPORT(tcgetpgrp)(int fd) {
    struct shim_tty *t = tty_of(fd);
    if (t) return t->fg_pgrp;
    RESOLVE(real_tcgetpgrp, tcgetpgrp);
    return real_tcgetpgrp(fd);
}

int IEC_EXPORT(tcsetpgrp)(int fd, pid_t pgrp) {
    struct shim_tty *t = tty_of(fd);
    if (t) {
        t->fg_pgrp = pgrp;
        return 0;
    }
    RESOLVE(real_tcsetpgrp, tcsetpgrp);
    return real_tcsetpgrp(fd, pgrp);
}

int IEC_EXPORT(ioctl)(int fd, unsigned long request, ...) {
    struct shim_tty *t = tty_of(fd);
    void *arg;
    va_list ap;

    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);

    if (t) {
        switch (request) {
        case TIOCGWINSZ:
            if (arg) *(struct winsize *)arg = t->ws;
            return 0;
        case TIOCSWINSZ:
            if (arg) t->ws = *(const struct winsize *)arg;
            return 0;
        case TCGETS:
            if (arg) *(struct termios *)arg = t->tio;
            return 0;
        case TCSETS:
        case TCSETSW:
        case TCSETSF:
            if (arg) t->tio = *(const struct termios *)arg;
            return 0;
#ifdef TIOCGPTN
        case TIOCGPTN:
            if (arg) *(int *)arg = t->pts_n;
            return 0;
#endif
#ifdef TIOCSCTTY
        case TIOCSCTTY:
            return 0;  /* pretend we took it as the controlling terminal */
#endif
        /* For callers that issue these ioctls directly instead of going through
         * tcsetpgrp()/tcgetpgrp(); glibc's own wrappers bypass this hook. */
#ifdef TIOCSPGRP
        case TIOCSPGRP:
            if (arg) t->fg_pgrp = *(const pid_t *)arg;
            return 0;
#endif
#ifdef TIOCGPGRP
        case TIOCGPGRP:
            if (arg) *(pid_t *)arg = t->fg_pgrp;
            return 0;
#endif
        default:
            break;
        }
    }

    RESOLVE(real_ioctl, ioctl);
    return real_ioctl(fd, request, arg);
}

int IEC_EXPORT(getpt)(void) {
    return open(PTMX_PATH, O_RDWR | O_NOCTTY);
}

int IEC_EXPORT(grantpt)(int fd) {
    /* Nothing to do: the server already owns the "slave" and hands it over on
     * open("/dev/pts/<n>"). */
    if (tty_of(fd)) return 0;
    RESOLVE(real_grantpt, grantpt);
    return real_grantpt(fd);
}

int IEC_EXPORT(unlockpt)(int fd) {
    if (tty_of(fd)) return 0;
    RESOLVE(real_unlockpt, unlockpt);
    return real_unlockpt(fd);
}

char *IEC_EXPORT(ptsname)(int fd) {
    static char buf[32];
    struct shim_tty *t = tty_of(fd);

    if (!t) {
        RESOLVE(real_ptsname, ptsname);
        return real_ptsname(fd);
    }
    snprintf(buf, sizeof buf, "/dev/pts/%d", t->pts_n);
    return buf;
}

int IEC_EXPORT(openpty)(int *amaster, int *aslave, char *name,
                        const struct termios *termp, const struct winsize *winp) {
    int master, slave;
    char *pname;

    master = getpt();
    if (master < 0) return -1;

    if (grantpt(master) < 0 || unlockpt(master) < 0) {
        close(master);
        return -1;
    }

    pname = ptsname(master);
    if (!pname) {
        close(master);
        return -1;
    }

    slave = open(pname, O_RDWR | O_NOCTTY);
    if (slave < 0) {
        close(master);
        return -1;
    }

    if (name) strcpy(name, pname);
    if (termp) tcsetattr(slave, TCSAFLUSH, termp);
    if (winp) ioctl(slave, TIOCSWINSZ, winp);

    *amaster = master;
    *aslave = slave;
    return 0;
}

int IEC_EXPORT(login_tty)(int fd) {
    if (setsid() < 0) return -1;
    if (ioctl(fd, TIOCSCTTY, NULL) < 0) return -1;
    if (dup2(fd, STDIN_FILENO) < 0) return -1;
    if (dup2(fd, STDOUT_FILENO) < 0) return -1;
    if (dup2(fd, STDERR_FILENO) < 0) return -1;
    tty_copy(fd, STDIN_FILENO);
    tty_copy(fd, STDOUT_FILENO);
    tty_copy(fd, STDERR_FILENO);

    /* The child almost always execs right after this, and the table above does
     * not cross that boundary; leave the pts number where the next image can
     * find it (see shim_resume_from_exec). */
    {
        struct shim_tty *t = tty_of(fd);
        if (t) {
            char v[16];
            snprintf(v, sizeof v, "%d", t->pts_n);
            setenv(IEC_TTY_ENV, v, 1);
            /* setsid() above gave this process its own group; the state block was
             * built in the parent and still names the parent's group, and acting
             * on that would signal the wrong processes. */
            t->fg_pgrp = getpgrp();
        }
    }

    if (fd > STDERR_FILENO) close(fd);
    return 0;
}

/* forkpty = openpty + fork + login_tty */
pid_t IEC_EXPORT(forkpty)(int *amaster, char *name,
                          const struct termios *termp,
                          const struct winsize *winp) {
    int master = -1, slave = -1;
    pid_t pid;

    if (openpty(&master, &slave, name, termp, winp) < 0) return -1;

    pid = fork();
    if (pid < 0) {
        int saved = errno;
        close(master);
        close(slave);
        errno = saved;
        return -1;
    }
    if (pid == 0) {
        close(master);
        if (login_tty(slave) < 0) _exit(1);
        return 0;
    }

    *amaster = master;
    close(slave);
    return pid;
}
