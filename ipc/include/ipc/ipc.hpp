// include/ipc/ipc.hpp

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace ipc {

// ─── Message ──────────────────────────────────────────────────────────────────

enum class MessageType : uint8_t {
    JSON   = 0,
    Binary = 1,
};

struct Message {
    MessageType          type;
    std::vector<uint8_t> payload;

    nlohmann::json           json()   const;
    std::span<const uint8_t> binary() const;
};

// ─── Error ────────────────────────────────────────────────────────────────────

enum class ErrorCode {
    ConnectionRefused,
    ConnectionLost,
    BadFrame,
    PayloadTooLarge,
    SendFailed,
    ListenFailed,
};

struct Error {
    ErrorCode   code;
    std::string message;
};

// ─── In-process registration ──────────────────────────────────────────────────

// Call before constructing Server in single binary mode.
// Enables in-process transport instead of socket or pipe.
void register_inprocess(const std::string& channel_id);
void unregister_inprocess(const std::string& channel_id);

// ─── Server ───────────────────────────────────────────────────────────────────

class Server {
public:
    explicit Server(std::string channel_id);
    ~Server();

    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;

    void on_connect(std::function<void()> fn);
    void on_disconnect(std::function<void()> fn);
    void on_message(std::function<void(Message)> fn);
    void on_error(std::function<void(Error)> fn);

    void listen(); // non-blocking — begins accepting on a background thread
    void stop();

    void send(const nlohmann::json& body);
    void send(std::vector<uint8_t> data);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ─── Client ───────────────────────────────────────────────────────────────────

class Client {
public:
    explicit Client(std::string channel_id);
    ~Client();

    Client(const Client&)            = delete;
    Client& operator=(const Client&) = delete;

    void on_connect(std::function<void()> fn);
    void on_disconnect(std::function<void()> fn);
    void on_message(std::function<void(Message)> fn);
    void on_error(std::function<void(Error)> fn);

    void connect(); // non-blocking — connects on a background thread
    void stop();

    void send(const nlohmann::json& body);
    void send(std::vector<uint8_t> data);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ipc