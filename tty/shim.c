// tty/shim.c — forkpty interception probe
//
// Purpose: verify that IEC's two injection paths (INT_WARP / INT_PRELOAD) can really intercept the
// client's forkpty call and forward the request to the out-of-process tty_server (see tty/tty_server.cpp).
// The server's OP_FORKPTY currently only returns a placeholder string, so we still return failure
// after getting the reply, in order to observe how weston-terminal behaves after forkpty fails.

#include "intercept.h"
#include "sock_client.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#define TTY_SOCK_PATH "/tmp/tty_service.sock"

/* Keep in sync with enum Op in tty/tty_server.h */
enum { OP_FORKPTY = 3 };

// forkpty = openpty + fork + login_tty
pid_t IEC_EXPORT(forkpty)(int *amaster, char *name,
                          const struct termios *termp,
                          const struct winsize *winp)
{
	(void)name;
	(void)termp;
	(void)winp;

	// stderr is unbuffered, and it does not depend on whether the caller emits a newline, so the log won't be swallowed
	fprintf(stderr, "[hook] forkpty intercepted (amaster=%p", (void *)amaster);
	if (amaster)
		fprintf(stderr, ", *amaster=%d", *amaster);
	fprintf(stderr, ")\n");

	sock_client c;
	if (sock_connect(&c, TTY_SOCK_PATH) < 0) {
		fprintf(stderr, "[hook] connect %s failed: %s\n",
		        TTY_SOCK_PATH, strerror(errno));
		errno = EPERM;
		return -1;
	}

	sock_reply r;
	int rc = sock_call(&c, OP_FORKPTY, NULL, 0, &r);
	if (rc < 0) {
		fprintf(stderr, "[hook] call OP_FORKPTY failed: %s\n", strerror(errno));
		sock_close(&c);
		errno = EPERM;
		return -1;
	}
	fprintf(stderr, "[hook] server %s: %s\n", rc ? "error" : "reply", r.body);
	sock_reply_free(&r);
	sock_close(&c);

	// forkpty failure can only return -1 and set errno: returning -2/-3 would be used by the caller as a pid
	errno = EPERM;
	return -1;
}
