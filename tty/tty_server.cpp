// tty/tty_server.cpp — TtyServer implementation
//
// Faking a pty: the sandboxed client cannot open /dev/ptmx, so we hand it a
// socketpair instead. The two ends behave like a pty pair (a byte stream that is
// readable and writable from both sides), which is all the client needs:
//
//   socketpair(sv)  ->  sv[0] = master, goes to the client right away
//                       sv[1] = slave,  kept here until the client opens
//                                       /dev/pts/<n>, then handed over
//
// The slave end is only held so that open("/dev/pts/<n>") has something to give
// back; once it is handed over we drop our copy, so that closing either end in
// the client is visible to the other one (EOF/HUP), which a terminal needs.
#include "tty_server.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>

namespace {

constexpr int kMaxPtys = 64;

struct Pty {
    bool used = false;
    int pts_n = 0;        // the "/dev/pts/<n>" number handed to the client
    int slave_fd = -1;    // our copy of the slave end, until OPENPTS takes it over
};

Pty g_pty[kMaxPtys];
int g_next_pts = 0;

Pty* allocPty() {
    for (int i = 0; i < kMaxPtys; i++)
        if (!g_pty[i].used) return &g_pty[i];
    return nullptr;
}

Pty* findByPts(int pts_n) {
    for (int i = 0; i < kMaxPtys; i++)
        if (g_pty[i].used && g_pty[i].pts_n == pts_n) return &g_pty[i];
    return nullptr;
}

}  // namespace

TtyServer::TtyServer(const std::string& path) : SockServer(path) {
    using namespace std::placeholders;
    registerHandler(OP_PING,     std::bind(&TtyServer::onPing, this, _1, _2));
    registerHandler(OP_PONG,     std::bind(&TtyServer::onPong, this, _1, _2));
    registerHandler(OP_FORKPTY,  std::bind(&TtyServer::onForkpty, this, _1, _2));
    registerHandler(OP_OPENPTMX, std::bind(&TtyServer::onOpenPtmx, this, _1, _2));
    registerHandler(OP_OPENPTS,  std::bind(&TtyServer::onOpenPts, this, _1, _2));
}

bool TtyServer::onPing(const Packet& req, Reply& reply) {
    reply.body = req.body;
    return true;
}

bool TtyServer::onPong(const Packet& req, Reply& reply) {
    // The client never sends a pong; this only occupies the op number.
    (void)req;
    (void)reply;
    return true;
}

bool TtyServer::onForkpty(const Packet& req, Reply& reply) {
    // Reserved: the shim builds forkpty() out of openpty() + fork() + login_tty()
    // on the client side, so this is not called.
    (void)req;
    putStr(reply.body, "Not implemented for now!\n");
    return true;
}

bool TtyServer::onOpenPtmx(const Packet& req, Reply& reply) {
    (void)req;

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        fprintf(stderr, "[server] socketpair: %s\n", strerror(errno));
        return false;
    }

    Pty* e = allocPty();
    if (!e) {
        fprintf(stderr, "[server] openptmx: out of pty slots\n");
        close(sv[0]);
        close(sv[1]);
        return false;
    }

    e->used = true;
    e->pts_n = g_next_pts++;
    e->slave_fd = sv[1];

    // The client needs the pts number to build "/dev/pts/<n>" later.
    put32(reply.body, static_cast<uint32_t>(e->pts_n));
    // The framework closes sv[0] once the kernel has taken its own reference.
    reply.fds.push_back(sv[0]);

    fprintf(stderr, "[server] /dev/ptmx -> pts/%d (master handed over)\n", e->pts_n);
    return true;
}

bool TtyServer::onOpenPts(const Packet& req, Reply& reply) {
    if (req.body.size() < 4) return false;

    int pts_n = static_cast<int>(get32(req.body.data()));
    Pty* e = findByPts(pts_n);
    if (!e) {
        fprintf(stderr, "[server] openpts: pts/%d not found\n", pts_n);
        return false;
    }

    int fd = dup(e->slave_fd);
    if (fd < 0) {
        fprintf(stderr, "[server] openpts: dup: %s\n", strerror(errno));
        return false;
    }
    reply.fds.push_back(fd);

    // Both ends now live in the client, so let go of ours.
    close(e->slave_fd);
    e->slave_fd = -1;
    e->used = false;

    fprintf(stderr, "[server] /dev/pts/%d -> slave handed over\n", pts_n);
    return true;
}
