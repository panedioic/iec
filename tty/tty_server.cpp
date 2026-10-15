// tty/tty_server.cpp — TtyServer implementation
#include "tty_server.h"

#include <functional>

TtyServer::TtyServer(const std::string& path) : SockServer(path) {
    using namespace std::placeholders;
    registerHandler(OP_PING, std::bind(&TtyServer::onPing, this, _1, _2));
    registerHandler(OP_PONG, std::bind(&TtyServer::onPong, this, _1, _2));
    registerHandler(OP_FORKPTY, std::bind(&TtyServer::onForkpty, this, _1, _2));
}

bool TtyServer::onPing(const Packet& req, std::vector<uint8_t>& reply) {
    reply = req.body;
    return true;
}

bool TtyServer::onPong(const Packet& req, std::vector<uint8_t>& reply) {
    // The client never sends a pong on its own; this just reserves the op number
    (void)req;
    (void)reply;
    return true;
}

bool TtyServer::onForkpty(const Packet& req, std::vector<uint8_t>& reply) {
    (void)req;
    // TODO: real implementation: openpty + fork on the server, and send the master fd back via SCM_RIGHTS
    putStr(reply, "Not implemented for now!\n");
    return true;
}
