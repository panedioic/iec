// exec/exec_server.h — ExecServer declaration
//
// Implementation in exec/exec_server.cpp. main.cpp only needs this header.
#pragma once

#include <poll.h>

#include <cstdint>
#include <string>
#include <vector>

#include "sock_operation.h"
#include "sock_server.h"

class ExecServer : public SockServer {
public:
    explicit ExecServer(const std::string& path);

private:
    bool onPing(const Packet& req, Reply& reply);
    bool onPong(const Packet& req, Reply& reply);
    bool onExecute(const Packet& req, Reply& reply);
    bool onWait(const Packet& req, Reply& reply);
    bool onSignal(const Packet& req, Reply& reply);

    // Reaps children that have exited since the previous pass of the event loop;
    // see the note above the definition for why this is a loop hook rather than a
    // SIGCHLD handler.
    void onTick() override;
};
