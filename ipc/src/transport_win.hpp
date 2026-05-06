// src/transport_win.hpp

#pragma once

#if defined(_WIN32)

#include "transport.hpp"

#include <atomic>
#include <memory>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace ipc::internal {

class WinTransport final : public Transport {
public:
    explicit WinTransport(HANDLE pipe);
    ~WinTransport() override;

    void   write(std::span<const uint8_t> data) override;
    size_t read(std::span<uint8_t> buf)         override;
    void   close()                              override;
    bool   is_connected() const                 override;

private:
    HANDLE            pipe_;
    std::atomic<bool> connected_{ true };
};

std::string                   win_pipe_name(const std::string& channel_id);
std::unique_ptr<WinTransport> win_listen(const std::string& channel_id);
std::unique_ptr<WinTransport> win_connect(const std::string& channel_id);

} // namespace ipc::internal

#endif // _WIN32