// src/transport_unix.cpp

#if !defined(_WIN32)

#include "transport_unix.hpp"

#include <stdexcept>
#include <cstring>
#include <cerrno>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace ipc::internal {

// ─── Path ─────────────────────────────────────────────────────────────────────

std::string unix_socket_path(const std::string& channel_id) {
    return "/tmp/arc-ipc-" + channel_id;
}

// ─── UnixTransport ────────────────────────────────────────────────────────────

UnixTransport::UnixTransport(int fd) : fd_(fd) {}

UnixTransport::~UnixTransport() {
    close();
}

void UnixTransport::write(std::span<const uint8_t> data) {
    size_t total = 0;
    while (total < data.size()) {
        ssize_t n = ::write(fd_, data.data() + total, data.size() - total);
        if (n <= 0) {
            connected_ = false;
            return;
        }
        total += static_cast<size_t>(n);
    }
}

size_t UnixTransport::read(std::span<uint8_t> buf) {
    size_t total = 0;
    while (total < buf.size()) {
        ssize_t n = ::read(fd_, buf.data() + total, buf.size() - total);
        if (n <= 0) {
            connected_ = false;
            return 0;
        }
        total += static_cast<size_t>(n);
    }
    return total;
}

void UnixTransport::close() {
    if (connected_.exchange(false)) {
        ::close(fd_);
    }
}

bool UnixTransport::is_connected() const {
    return connected_;
}

// ─── Listen ───────────────────────────────────────────────────────────────────

std::unique_ptr<UnixTransport> unix_listen(const std::string& channel_id) {
    std::string path = unix_socket_path(channel_id);

    int server_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0)
        throw std::runtime_error("ipc: socket() failed: " + std::string(strerror(errno)));

    ::unlink(path.c_str()); // remove stale socket

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(server_fd);
        throw std::runtime_error("ipc: bind() failed: " + std::string(strerror(errno)));
    }

    if (::listen(server_fd, 1) < 0) {
        ::close(server_fd);
        throw std::runtime_error("ipc: listen() failed: " + std::string(strerror(errno)));
    }

    int client_fd = ::accept(server_fd, nullptr, nullptr); // blocks until client connects
    ::close(server_fd);

    if (client_fd < 0)
        throw std::runtime_error("ipc: accept() failed: " + std::string(strerror(errno)));

    return std::make_unique<UnixTransport>(client_fd);
}

// ─── Connect ──────────────────────────────────────────────────────────────────

std::unique_ptr<UnixTransport> unix_connect(const std::string& channel_id) {
    std::string path = unix_socket_path(channel_id);

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        throw std::runtime_error("ipc: socket() failed: " + std::string(strerror(errno)));

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        throw std::runtime_error("ipc: connect() failed: " + std::string(strerror(errno)));
    }

    return std::make_unique<UnixTransport>(fd);
}

} // namespace ipc::internal

#endif // !_WIN32