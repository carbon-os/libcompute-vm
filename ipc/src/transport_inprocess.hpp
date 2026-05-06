// src/transport_inprocess.hpp

#pragma once

#include "transport.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ipc::internal {

// Shared queue between the two ends of an in-process channel.
struct InProcessPipe {
    std::mutex                       mutex;
    std::condition_variable          cv;
    std::deque<std::vector<uint8_t>> queue; // each entry is one complete framed message
    bool                             closed{ false };
};

// One end of an in-process channel.
// Writes complete framed messages to tx_, reads from rx_.
class InProcessTransport final : public Transport {
public:
    InProcessTransport(std::shared_ptr<InProcessPipe> rx,
                       std::shared_ptr<InProcessPipe> tx);
    ~InProcessTransport() override;

    void   write(std::span<const uint8_t> data) override;
    size_t read(std::span<uint8_t> buf)         override;
    void   close()                              override;
    bool   is_connected() const                 override;

private:
    std::shared_ptr<InProcessPipe> rx_;
    std::shared_ptr<InProcessPipe> tx_;
    std::atomic<bool>              connected_{ true };

    // Partial-read state — serves bytes from a dequeued message across
    // multiple read() calls until the message is fully consumed.
    std::vector<uint8_t> current_;
    size_t               current_pos_{ 0 };
};

// Registry
void register_inprocess(const std::string& channel_id);
void unregister_inprocess(const std::string& channel_id);
bool is_inprocess(const std::string& channel_id);

// Acquire transports — server calls first, client calls second.
std::unique_ptr<InProcessTransport> inprocess_listen(const std::string& channel_id);
std::unique_ptr<InProcessTransport> inprocess_connect(const std::string& channel_id);

} // namespace ipc::internal