// common/sock_server.h - 
//
// Protocol (all multi-byte integers are in network byte order/big endian):
//     [0-3] Total packet length (including header)
//     [4-5] Header length (currently 16)
//     [6-7] Protocol version
//     [8-11] Operation type
//     [12-15] Reserved
//     [16..] Packet content
//
// Declarations only; the definitions live in common/sock_server.cpp.
#pragma once

#include <cstddef>
#include <csignal>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

constexpr uint32_t kHeaderLen = 16;
constexpr uint16_t kVersion = 1;
constexpr uint32_t kMaxPacketLen = 1u << 20;
constexpr uint32_t kOpError = 0xFFFFFFFF;

struct Packet {
    uint32_t length = 0;      // Total length
    uint16_t header_len = 0;
    uint16_t version = 0;
    uint32_t op = 0;
    uint32_t reserved = 0;
    std::vector<uint8_t> body;  // Content
};

void put32(std::vector<uint8_t>& v, uint32_t x);
void put16(std::vector<uint8_t>& v, uint16_t x);
// The body boundary is given by length in the packet header, so no length prefix is added here:
// the raw bytes are appended as-is. sendError's error text and OP_PING's echo follow this rule, and the client appends '\0' itself on receipt.
void putStr(std::vector<uint8_t>& v, const std::string& s);
uint32_t get32(const uint8_t* p);
uint16_t get16(const uint8_t* p);
std::string getStr(const uint8_t* p, size_t len);
// Convenience overload: use the whole packet body as a string
std::string getStr(const Packet& p);

std::vector<uint8_t> encode(uint32_t op, const std::vector<uint8_t>& body);

class SockServer {
public:
    // Handler: read req and write the response content into reply; returning false means the handler failed (an error packet is sent back)
    using Handler = std::function<bool(const Packet& req, std::vector<uint8_t>& reply)>;

    explicit SockServer(std::string path);
    virtual ~SockServer();
    SockServer(const SockServer&) = delete;
    SockServer& operator=(const SockServer&) = delete;

    // Called by derived classes: register a handler for an operation type
    void registerHandler(uint32_t op, Handler h);

    // Create the socket and start listening
    void start();

    // Single-threaded poll event loop, until stop()
    void run();

    void stop();

private:
    // Extract all complete packets from the buffer and dispatch them; returning false means the connection should be closed
    bool drain(int fd, std::vector<uint8_t>& b);

    static const char* validate(const Packet& p);

    void dispatch(int fd, const Packet& req);

    void sendError(int fd, const char* msg);

    static void sendAll(int fd, const std::vector<uint8_t>& d);

    std::string path_;
    int listen_fd_ = -1;
    bool bound_ = false;   // whether bind succeeded (decides whether to remove the socket file on destruction)
    // Read and written by the signal handler, so use sig_atomic_t rather than std::atomic
    volatile sig_atomic_t running_ = 1;
    std::map<uint32_t, Handler> handlers_;
};
