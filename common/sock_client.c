// common/sock_client.c
#include "sock_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int read_full(int fd, void *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t r = read(fd, (char *)buf + off, n - off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        off += (size_t)r;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, (const char *)buf + off, n - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

/* Pulls any SCM_RIGHTS payload out of a received message into the reply. */
static void collect_fds(struct msghdr *msg, sock_reply *out) {
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(msg); cm != NULL; cm = CMSG_NXTHDR(msg, cm)) {
        if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) continue;
        if (cm->cmsg_len < CMSG_LEN(0)) continue;
        size_t n = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        const int *received = (const int *)CMSG_DATA(cm);
        for (size_t i = 0; i < n; i++) {
            if (out->nfds < SOCK_MAX_FDS) {
                out->fds[out->nfds++] = received[i];
            } else {
                /* No room left: close it rather than leak it. */
                close(received[i]);
            }
        }
    }
}

/* Reads the 16-byte reply header. This has to use recvmsg, because any fds the
 * server sends travel attached to the first byte of the reply. */
static int recv_header(int fd, uint8_t *h, sock_reply *out) {
    size_t got = 0;
    while (got < SOCK_HDR_LEN) {
        struct iovec iov;
        struct msghdr msg;
        char cbuf[CMSG_SPACE(sizeof(int) * SOCK_MAX_FDS)];

        memset(&msg, 0, sizeof msg);
        iov.iov_base = h + got;
        iov.iov_len = SOCK_HDR_LEN - got;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cbuf;
        msg.msg_controllen = sizeof cbuf;

#ifdef MSG_CMSG_CLOEXEC
        ssize_t r = recvmsg(fd, &msg, MSG_CMSG_CLOEXEC);
#else
        ssize_t r = recvmsg(fd, &msg, 0);
#endif
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        collect_fds(&msg, out);
        got += (size_t)r;
    }
    return 0;
}

void sock_reply_free(sock_reply *r) {
    int i;
    if (!r) return;
    free(r->body);
    r->body = NULL;
    for (i = 0; i < r->nfds; i++) {
        if (r->fds[i] >= 0) close(r->fds[i]);
        r->fds[i] = -1;
    }
    r->nfds = 0;
}

int sock_reply_take_fd(sock_reply *r, int i) {
    int fd;
    if (!r || i < 0 || i >= r->nfds) return -1;
    fd = r->fds[i];
    r->fds[i] = -1;
    return fd;
}

int sock_connect(sock_client *c, const char *path) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof addr.sun_path) return -1;
    strcpy(addr.sun_path, path);

    c->version = SOCK_VERSION;
    c->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (c->fd < 0) return -1;
    if (connect(c->fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(c->fd);
        c->fd = -1;
        return -1;
    }
    return 0;
}

int sock_call(sock_client *c, uint32_t op, const void *body, uint32_t len, sock_reply *out) {
    uint8_t *pkt;
    uint32_t total = htonl(SOCK_HDR_LEN + len);
    uint16_t hl = htons(SOCK_HDR_LEN), ver = htons(c->version);
    uint32_t o = htonl(op), rsv = 0;
    uint8_t h[SOCK_HDR_LEN];
    uint32_t rtotal, rop, blen;
    uint16_t rhl;

    if (!out) return -1;
    out->op = 0;
    out->len = 0;
    out->body = NULL;
    out->nfds = 0;
    memset(out->fds, 0xff, sizeof out->fds);

    /* --- send --- */
    pkt = malloc(SOCK_HDR_LEN + (len ? len : 1));
    if (!pkt) return -1;
    memcpy(pkt, &total, 4);
    memcpy(pkt + 4, &hl, 2);
    memcpy(pkt + 6, &ver, 2);
    memcpy(pkt + 8, &o, 4);
    memcpy(pkt + 12, &rsv, 4);
    if (len && body) memcpy(pkt + SOCK_HDR_LEN, body, len);
    if (write_full(c->fd, pkt, SOCK_HDR_LEN + len) < 0) {
        free(pkt);
        return -1;
    }
    free(pkt);

    /* --- receive --- */
    if (recv_header(c->fd, h, out) < 0) {
        sock_reply_free(out);
        return -1;
    }
    memcpy(&rtotal, h, 4);   rtotal = ntohl(rtotal);
    memcpy(&rhl, h + 4, 2);  rhl = ntohs(rhl);
    memcpy(&rop, h + 8, 4);  rop = ntohl(rop);
    if (rhl < SOCK_HDR_LEN || rtotal < rhl || rtotal > (1u << 20)) {
        sock_reply_free(out);
        return -1;
    }

    /* Skip any extended header the server may have added. */
    for (uint32_t left = rhl - SOCK_HDR_LEN; left > 0;) {
        uint8_t skip[64];
        uint32_t n = left < sizeof skip ? left : (uint32_t)sizeof skip;
        if (read_full(c->fd, skip, n) < 0) {
            sock_reply_free(out);
            return -1;
        }
        left -= n;
    }

    blen = rtotal - rhl;
    out->body = malloc(blen + 1);
    if (!out->body) {
        sock_reply_free(out);
        return -1;
    }
    if (blen && read_full(c->fd, out->body, blen) < 0) {
        sock_reply_free(out);
        return -1;
    }
    out->body[blen] = '\0';
    out->len = blen;
    out->op = rop;
    return rop == SOCK_OP_ERROR ? 1 : 0;
}

void sock_close(sock_client *c) {
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}
