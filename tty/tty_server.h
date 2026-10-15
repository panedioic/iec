// tty/tty_server.h — TtyServer declaration
//
// See tty/tty_server.cpp for the implementation. main.cpp only depends on this header and does not need to know the implementation details.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sock_server.h"

enum Op : uint32_t {
    OP_PING = 1,
    OP_PONG = 2,
    OP_FORKPTY = 3,
};

class TtyServer : public SockServer {
public:
    explicit TtyServer(const std::string& path);

private:
    bool onPing(const Packet& req, std::vector<uint8_t>& reply);
    bool onPong(const Packet& req, std::vector<uint8_t>& reply);
    bool onForkpty(const Packet& req, std::vector<uint8_t>& reply);
};
