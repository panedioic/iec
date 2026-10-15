// main.cpp — tty server entry point
#include <csignal>
#include <cstdio>
#include <exception>

#include "tty/tty_server.h"

static TtyServer* g_tty_server = nullptr;

static void onSignal(int) {
    if (g_tty_server) g_tty_server->stop();
}

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "/tmp/tty_service.sock";
    try {
        TtyServer srv(path);
        g_tty_server = &srv;
        signal(SIGINT, onSignal);
        signal(SIGTERM, onSignal);
        srv.start();
        srv.run();
    } catch (const std::exception& e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
