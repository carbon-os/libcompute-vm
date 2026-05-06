// src/channel.hpp

#pragma once

#include <ipc/ipc.hpp>
#include "transport.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace ipc::internal {

class Channel {
public:
    explicit Channel(std::unique_ptr<Transport> transport);
    ~Channel();

    Channel(const Channel&)            = delete;
    Channel& operator=(const Channel&) = delete;

    void on_disconnect(std::function<void()> fn)     { on_disconnect_ = std::move(fn); }
    void on_message(std::function<void(Message)> fn) { on_message_    = std::move(fn); }
    void on_error(std::function<void(Error)> fn)     { on_error_      = std::move(fn); }

    void start(); // launches read loop thread
    void stop();

    void send(const nlohmann::json& body);
    void send(std::vector<uint8_t> data);

private:
    void read_loop();
    void send_raw(MessageType type, std::span<const uint8_t> payload);

    std::unique_ptr<Transport>   transport_;
    std::jthread                 read_thread_;
    std::mutex                   write_mutex_;

    std::function<void()>        on_disconnect_;
    std::function<void(Message)> on_message_;
    std::function<void(Error)>   on_error_;
};

} // namespace ipc::internal