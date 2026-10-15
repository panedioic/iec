// common/sock_client.c
#include "sock_client.h"

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int read_full(int fd, void *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t r = read(fd, (char *)buf + off, n - off);
        if (r <= 0) return -1;
        off += (size_t)r;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, (const char *)buf + off, n - off);
        if (w <= 0) return -1;
        off += (size_t)w;
    }
    return 0;
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
    /* Send */
    uint8_t *pkt = malloc(SOCK_HDR_LEN + len);
    if (!pkt) return -1;
    uint32_t total = htonl(SOCK_HDR_LEN + len);
    uint16_t hl = htons(SOCK_HDR_LEN), ver = htons(c->version);
    uint32_t o = htonl(op), rsv = 0;
    memcpy(pkt, &total, 4);
    memcpy(pkt + 4, &hl, 2);
    memcpy(pkt + 6, &ver, 2);
    memcpy(pkt + 8, &o, 4);
    memcpy(pkt + 12, &rsv, 4);
    if (len) memcpy(pkt + SOCK_HDR_LEN, body, len);
    int rc = write_full(c->fd, pkt, SOCK_HDR_LEN + len);
    free(pkt);
    if (rc < 0) return -1;

    /* Receive */
    uint8_t h[SOCK_HDR_LEN];
    if (read_full(c->fd, h, SOCK_HDR_LEN) < 0) return -1;
    uint32_t rtotal, rop;
    uint16_t rhl;
    memcpy(&rtotal, h, 4);   rtotal = ntohl(rtotal);
    memcpy(&rhl, h + 4, 2);  rhl = ntohs(rhl);
    memcpy(&rop, h + 8, 4);  rop = ntohl(rop);
    if (rhl < SOCK_HDR_LEN || rtotal < rhl) return -1;

    /* Skip any extended header that may be present */
    for (uint32_t left = rhl - SOCK_HDR_LEN; left > 0;) {
        uint8_t skip[64];
        uint32_t n = left < sizeof skip ? left : (uint32_t)sizeof skip;
        if (read_full(c->fd, skip, n) < 0) return -1;
        left -= n;
    }

    uint32_t blen = rtotal - rhl;
    uint8_t *rb = malloc(blen + 1);
    if (!rb) return -1;
    if (blen && read_full(c->fd, rb, blen) < 0) { free(rb); return -1; }
    rb[blen] = '\0';

    out->op = rop;
    out->len = blen;
    out->body = rb;
    return rop == SOCK_OP_ERROR ? 1 : 0;
}

void sock_reply_free(sock_reply *r) {
    free(r->body);
    r->body = NULL;
}

void sock_close(sock_client *c) {
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}
