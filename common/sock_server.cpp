// common/sock_server.cpp - implementation of the UDS server framework declared in sock_server.h
#include "sock_server.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>

void put32(std::vector<uint8_t>& v, uint32_t x) {
    uint32_t n = htonl(x);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&n);
    v.insert(v.end(), p, p + 4);
}

void put16(std::vector<uint8_t>& v, uint16_t x) {
    uint16_t n = htons(x);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&n);
    v.insert(v.end(), p, p + 2);
}

void putStr(std::vector<uint8_t>& v, const std::string& s) {
    v.insert(v.end(), s.begin(), s.end());
}

uint32_t get32(const uint8_t* p) { uint32_t n; memcpy(&n, p, 4); return ntohl(n); }
uint16_t get16(const uint8_t* p) { uint16_t n; memcpy(&n, p, 2); return ntohs(n); }

std::string getStr(const uint8_t* p, size_t len) {
    return std::string(reinterpret_cast<const char*>(p), len);
}

std::string getStr(const Packet& p) {
    if (p.body.empty()) return std::string();
    return getStr(p.body.data(), p.body.size());
}

std::vector<uint8_t> encode(uint32_t op, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> out;
    out.reserve(kHeaderLen + body.size());
    put32(out, kHeaderLen + static_cast<uint32_t>(body.size()));
    put16(out, kHeaderLen);
    put16(out, kVersion);
    put32(out, op);
    put32(out, 0);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

SockServer::SockServer(std::string path) : path_(std::move(path)) {}

SockServer::~SockServer() {
    if (listen_fd_ >= 0) close(listen_fd_);
    // Only an instance that actually bound successfully removes the socket file, otherwise it would wrongly delete one someone else is using
    if (bound_ && !path_.empty()) unlink(path_.c_str());
}

void SockServer::registerHandler(uint32_t op, Handler h) {
    handlers_[op] = std::move(h);
}

void SockServer::start() {
    sockaddr_un addr{};
    if (path_.size() >= sizeof(addr.sun_path)) throw std::runtime_error("path too long");
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path_.c_str(), path_.size() + 1);

    // First probe whether a live server already owns the path: if connect succeeds one exists, and
    // unlinking it would delete its socket file, leaving two servers with clients connecting by luck; only a failed connect means a stale file.
    int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe >= 0) {
        bool alive = connect(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
        close(probe);
        if (alive) throw std::runtime_error("already running on " + path_);
    }

    listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) throw std::runtime_error(std::string("socket: ") + strerror(errno));
    unlink(path_.c_str());  // clean up a leftover old socket file
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        listen(listen_fd_, 16) < 0)
        throw std::runtime_error(std::string("bind/listen: ") + strerror(errno));
    bound_ = true;
    // Logging goes to stderr: the server is usually put in the background, and when stdout is
    // redirected to a file it is fully buffered, so this line would stay stuck in the buffer unseen
    fprintf(stderr, "[server] listening on %s\n", path_.c_str());
}

void SockServer::run() {
    static constexpr size_t kMaxClients = 64;

    std::vector<pollfd> fds{{listen_fd_, POLLIN, 0}};
    std::map<int, std::vector<uint8_t>> bufs;  // receive buffer per connection (handles coalesced/split packets)

    while (running_) {
        int n = poll(fds.data(), fds.size(), 500);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) continue;

        std::vector<int> to_close;
        size_t cnt = fds.size();
        for (size_t i = 0; i < cnt; ++i) {
            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            int fd = fds[i].fd;
            if (fd == listen_fd_) {
                int c = accept(listen_fd_, nullptr, nullptr);
                if (c < 0) {
                    // A failed accept leaves the connection pending, so poll reports it
                    // again right away and the loop would spin at 100% CPU (EMFILE is the
                    // usual cause). Back off briefly instead of retrying immediately.
                    if (errno != EINTR) {
                        fprintf(stderr, "[server] accept: %s\n", strerror(errno));
                        usleep(10 * 1000);
                    }
                } else if (fds.size() - 1 >= kMaxClients) {
                    // Without a cap, one client can exhaust the server's memory and fds.
                    fprintf(stderr, "[server] too many clients, dropping connection\n");
                    close(c);
                } else {
                    fds.push_back({c, POLLIN, 0});
                    bufs[c];
                }
                continue;
            }
            uint8_t tmp[4096];
            ssize_t r = recv(fd, tmp, sizeof(tmp), 0);
            if (r <= 0) { if (r < 0 && errno == EINTR) continue; to_close.push_back(fd); continue; }
            auto& b = bufs[fd];
            b.insert(b.end(), tmp, tmp + r);
            if (!drain(fd, b)) to_close.push_back(fd);
        }
        for (int fd : to_close) {
            close(fd);
            bufs.erase(fd);
            for (size_t i = 0; i < fds.size(); ++i)
                if (fds[i].fd == fd) { fds.erase(fds.begin() + i); break; }
        }
    }
    for (size_t i = 1; i < fds.size(); ++i) close(fds[i].fd);
}

void SockServer::stop() { running_ = false; }

bool SockServer::drain(int fd, std::vector<uint8_t>& b) {
    while (b.size() >= 4) {
        uint32_t total = get32(b.data());
        // Use the length field for a basic check first, to avoid waiting for a packet that will never become complete
        if (total < kHeaderLen || total > kMaxPacketLen) {
            sendError(fd, "invalid packet length");
            return false;
        }
        if (b.size() < total) break;  // partial packet, keep waiting

        Packet p;
        p.length = total;
        p.header_len = get16(&b[4]);
        p.version = get16(&b[6]);
        p.op = get32(&b[8]);
        p.reserved = get32(&b[12]);
        const char* err = validate(p);
        if (err) { sendError(fd, err); return false; }
        p.body.assign(b.begin() + p.header_len, b.begin() + total);
        b.erase(b.begin(), b.begin() + total);
        dispatch(fd, p);
    }
    return true;
}

const char* SockServer::validate(const Packet& p) {
    if (p.version != kVersion) return "unsupported version";
    if (p.header_len < kHeaderLen || p.header_len > p.length) return "invalid header length";
    return nullptr;
}

void SockServer::dispatch(int fd, const Packet& req) {
    auto it = handlers_.find(req.op);
    if (it == handlers_.end()) { sendError(fd, "unknown operation"); return; }
    std::vector<uint8_t> reply;
    if (!it->second(req, reply)) { sendError(fd, "handler failed"); return; }
    sendAll(fd, encode(req.op, reply));
}

void SockServer::sendError(int fd, const char* msg) {
    sendAll(fd, encode(kOpError, std::vector<uint8_t>(msg, msg + strlen(msg))));
}

void SockServer::sendAll(int fd, const std::vector<uint8_t>& d) {
    size_t off = 0;
    while (off < d.size()) {
        ssize_t w = send(fd, d.data() + off, d.size() - off, MSG_NOSIGNAL);
        if (w < 0) { if (errno == EINTR) continue; return; }
        // w == 0 should not happen on a stream socket; bail out rather than spin forever.
        if (w == 0) return;
        off += static_cast<size_t>(w);
    }
}
