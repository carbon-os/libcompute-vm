// src/channel.cpp

#include "channel.hpp"
#include "framing.hpp"

#include <cstring>

namespace ipc::internal {

Channel::Channel(std::unique_ptr<Transport> transport)
    : transport_(std::move(transport))
{}

Channel::~Channel() {
    stop();
}

void Channel::start() {
    read_thread_ = std::jthread([this](std::stop_token) {
        read_loop();
    });
}

void Channel::stop() {
    transport_->close();
    if (read_thread_.joinable())
        read_thread_.join();
}

// ─── Send ─────────────────────────────────────────────────────────────────────

void Channel::send(const nlohmann::json& body) {
    std::string serialised = body.dump();
    std::span<const uint8_t> payload(
        reinterpret_cast<const uint8_t*>(serialised.data()),
        serialised.size()
    );
    send_raw(MessageType::JSON, payload);
}

void Channel::send(std::vector<uint8_t> data) {
    send_raw(MessageType::Binary, std::span<const uint8_t>(data));
}

void Channel::send_raw(MessageType type, std::span<const uint8_t> payload) {
    FrameHeader hdr{};
    hdr.magic       = kMagic;
    hdr.version     = kVersion;
    hdr.type        = static_cast<uint8_t>(type);
    hdr.reserved    = 0;
    hdr.payload_len = static_cast<uint32_t>(payload.size());

    std::lock_guard lock(write_mutex_);

    transport_->write(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr)
    ));
    if (!payload.empty())
        transport_->write(payload);
}

// ─── Read loop ────────────────────────────────────────────────────────────────

void Channel::read_loop() {
    while (transport_->is_connected()) {
        // read header
        FrameHeader hdr{};
        size_t n = transport_->read(std::span<uint8_t>(
            reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr)
        ));

        if (n == 0) break; // disconnected

        // validate
        if (hdr.magic != kMagic || hdr.version != kVersion) {
            if (on_error_)
                on_error_({ ErrorCode::BadFrame, "invalid frame header" });
            break;
        }

        if (hdr.payload_len > kMaxPayloadSize) {
            if (on_error_)
                on_error_({ ErrorCode::PayloadTooLarge, "payload exceeds 64 MiB cap" });
            break;
        }

        // read payload
        std::vector<uint8_t> payload(hdr.payload_len);
        if (hdr.payload_len > 0) {
            n = transport_->read(std::span<uint8_t>(payload));
            if (n == 0) break;
        }

        if (on_message_) {
            Message msg;
            msg.type    = static_cast<MessageType>(hdr.type);
            msg.payload = std::move(payload);
            on_message_(std::move(msg));
        }
    }

    if (on_disconnect_)
        on_disconnect_();
}

} // namespace ipc::internal