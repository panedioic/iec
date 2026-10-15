// tty/tty_server.h — TtyServer declaration
//
// Implementation in tty/tty_server.cpp. main.cpp only needs this header.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sock_operation.h"
#include "sock_server.h"

class TtyServer : public SockServer {
public:
    explicit TtyServer(const std::string& path);

private:
    bool onPing(const Packet& req, Reply& reply);
    bool onPong(const Packet& req, Reply& reply);
    bool onForkpty(const Packet& req, Reply& reply);
    bool onOpenPtmx(const Packet& req, Reply& reply);
    bool onOpenPts(const Packet& req, Reply& reply);
};
