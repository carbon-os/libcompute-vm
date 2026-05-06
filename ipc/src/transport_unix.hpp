// src/transport_unix.hpp

#pragma once

#if !defined(_WIN32)

#include "transport.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace ipc::internal {

class UnixTransport final : public Transport {
public:
    explicit UnixTransport(int fd);
    ~UnixTransport() override;

    void   write(std::span<const uint8_t> data) override;
    size_t read(std::span<uint8_t> buf)         override;
    void   close()                              override;
    bool   is_connected() const                 override;

private:
    int               fd_;
    std::atomic<bool> connected_{ true };
};

std::string                    unix_socket_path(const std::string& channel_id);
std::unique_ptr<UnixTransport> unix_listen(const std::string& channel_id);
std::unique_ptr<UnixTransport> unix_connect(const std::string& channel_id);

} // namespace ipc::internal

#endif // !_WIN32