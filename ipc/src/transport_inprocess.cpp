// src/transport_inprocess.cpp

#include "transport_inprocess.hpp"

#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace ipc::internal {

// ─── Registry ─────────────────────────────────────────────────────────────────

struct InProcessEntry {
    std::shared_ptr<InProcessPipe> server_to_client;
    std::shared_ptr<InProcessPipe> client_to_server;
};

static std::mutex                                       s_registry_mutex;
static std::unordered_map<std::string, InProcessEntry> s_registry;

void register_inprocess(const std::string& channel_id) {
    std::lock_guard lock(s_registry_mutex);
    s_registry[channel_id] = {
        std::make_shared<InProcessPipe>(),
        std::make_shared<InProcessPipe>(),
    };
}

void unregister_inprocess(const std::string& channel_id) {
    std::lock_guard lock(s_registry_mutex);
    s_registry.erase(channel_id);
}

bool is_inprocess(const std::string& channel_id) {
    std::lock_guard lock(s_registry_mutex);
    return s_registry.contains(channel_id);
}

// ─── Acquire transports ───────────────────────────────────────────────────────

std::unique_ptr<InProcessTransport> inprocess_listen(const std::string& channel_id) {
    std::lock_guard lock(s_registry_mutex);
    auto it = s_registry.find(channel_id);
    if (it == s_registry.end())
        throw std::runtime_error("ipc: inprocess channel not registered: " + channel_id);

    // server reads from client_to_server, writes to server_to_client
    return std::make_unique<InProcessTransport>(
        it->second.client_to_server,
        it->second.server_to_client
    );
}

std::unique_ptr<InProcessTransport> inprocess_connect(const std::string& channel_id) {
    std::lock_guard lock(s_registry_mutex);
    auto it = s_registry.find(channel_id);
    if (it == s_registry.end())
        throw std::runtime_error("ipc: inprocess channel not registered: " + channel_id);

    // client reads from server_to_client, writes to client_to_server
    return std::make_unique<InProcessTransport>(
        it->second.server_to_client,
        it->second.client_to_server
    );
}

// ─── InProcessTransport ───────────────────────────────────────────────────────

InProcessTransport::InProcessTransport(std::shared_ptr<InProcessPipe> rx,
                                       std::shared_ptr<InProcessPipe> tx)
    : rx_(std::move(rx))
    , tx_(std::move(tx))
{}

InProcessTransport::~InProcessTransport() {
    close();
}

void InProcessTransport::write(std::span<const uint8_t> data) {
    if (!connected_) return;
    std::vector<uint8_t> msg(data.begin(), data.end());
    {
        std::lock_guard lock(tx_->mutex);
        if (tx_->closed) { connected_ = false; return; }
        tx_->queue.push_back(std::move(msg));
    }
    tx_->cv.notify_one();
}

size_t InProcessTransport::read(std::span<uint8_t> buf) {
    size_t total = 0;
    while (total < buf.size()) {
        // drain current dequeued message first
        if (current_pos_ < current_.size()) {
            size_t avail = current_.size() - current_pos_;
            size_t n     = std::min(avail, buf.size() - total);
            std::memcpy(buf.data() + total, current_.data() + current_pos_, n);
            current_pos_ += n;
            total        += n;
            continue;
        }

        // wait for next message
        std::unique_lock lock(rx_->mutex);
        rx_->cv.wait(lock, [&] {
            return !rx_->queue.empty() || rx_->closed;
        });

        if (rx_->queue.empty()) {
            connected_ = false;
            return 0;
        }

        current_     = std::move(rx_->queue.front());
        current_pos_ = 0;
        rx_->queue.pop_front();
    }
    return total;
}

void InProcessTransport::close() {
    if (!connected_.exchange(false)) return;
    {
        std::lock_guard lock(rx_->mutex);
        rx_->closed = true;
    }
    rx_->cv.notify_all();
}

bool InProcessTransport::is_connected() const {
    return connected_;
}

} // namespace ipc::internal