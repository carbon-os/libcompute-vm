// src/server.cpp

#include <ipc/ipc.hpp>
#include "channel.hpp"
#include "transport_inprocess.hpp"

#if defined(_WIN32)
#  include "transport_win.hpp"
#else
#  include "transport_unix.hpp"
#endif

#include <thread>

namespace ipc {

// ─── Message ──────────────────────────────────────────────────────────────────

nlohmann::json           Message::json()   const { return nlohmann::json::parse(payload); }
std::span<const uint8_t> Message::binary() const { return payload; }

// ─── In-process registration ──────────────────────────────────────────────────

void register_inprocess(const std::string& channel_id) {
    internal::register_inprocess(channel_id);
}

void unregister_inprocess(const std::string& channel_id) {
    internal::unregister_inprocess(channel_id);
}

// ─── Server::Impl ─────────────────────────────────────────────────────────────

struct Server::Impl {
    std::string channel_id;

    std::function<void()>           on_connect_cb;
    std::function<void()>           on_disconnect_cb;
    std::function<void(Message)>    on_message_cb;
    std::function<void(Error)>      on_error_cb;

    std::unique_ptr<internal::Channel> channel;
    std::jthread                       accept_thread;
};

// ─── Server ───────────────────────────────────────────────────────────────────

Server::Server(std::string channel_id)
    : impl_(std::make_unique<Impl>())
{
    impl_->channel_id = std::move(channel_id);
}

Server::~Server() {
    stop();
}

void Server::on_connect(std::function<void()> fn)          { impl_->on_connect_cb    = std::move(fn); }
void Server::on_disconnect(std::function<void()> fn)       { impl_->on_disconnect_cb = std::move(fn); }
void Server::on_message(std::function<void(Message)> fn)   { impl_->on_message_cb    = std::move(fn); }
void Server::on_error(std::function<void(Error)> fn)       { impl_->on_error_cb      = std::move(fn); }

void Server::listen() {
    impl_->accept_thread = std::jthread([this](std::stop_token) {
        try {
            // acquire transport — blocks until client connects
            std::unique_ptr<internal::Transport> transport;

            if (internal::is_inprocess(impl_->channel_id)) {
                transport = internal::inprocess_listen(impl_->channel_id);
            } else {
#if defined(_WIN32)
                transport = internal::win_listen(impl_->channel_id);
#else
                transport = internal::unix_listen(impl_->channel_id);
#endif
            }

            impl_->channel = std::make_unique<internal::Channel>(std::move(transport));

            impl_->channel->on_disconnect([this] {
                if (impl_->on_disconnect_cb) impl_->on_disconnect_cb();
            });
            impl_->channel->on_message([this](Message msg) {
                if (impl_->on_message_cb) impl_->on_message_cb(std::move(msg));
            });
            impl_->channel->on_error([this](Error err) {
                if (impl_->on_error_cb) impl_->on_error_cb(std::move(err));
            });

            impl_->channel->start();

            if (impl_->on_connect_cb) impl_->on_connect_cb();

        } catch (const std::exception& e) {
            if (impl_->on_error_cb)
                impl_->on_error_cb({ ErrorCode::ListenFailed, e.what() });
        }
    });
}

void Server::stop() {
    if (impl_->channel) impl_->channel->stop();
    if (impl_->accept_thread.joinable()) impl_->accept_thread.join();
}

void Server::send(const nlohmann::json& body) {
    if (impl_->channel) impl_->channel->send(body);
}

void Server::send(std::vector<uint8_t> data) {
    if (impl_->channel) impl_->channel->send(std::move(data));
}

} // namespace ipc