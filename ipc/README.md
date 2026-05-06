# libipc Architecture

## Overview

libipc is the inter-process communication layer for the arc framework. It
provides a clean, cross-platform channel abstraction over native IPC
transports. It handles connection lifecycle, message framing, and delivery —
nothing more.

libhost is always the listener. Go is always the connector. libipc satisfies
both roles through a symmetric API — the only difference is which side calls
`listen()` and which calls `connect()`.

---

## Platform Transports

| Platform | Transport |
|---|---|
| Linux | Unix domain socket |
| macOS | Unix domain socket |
| Windows | Named pipe |

The channel ID passed at construction is the only addressing mechanism. libipc
maps it to the correct platform path or pipe name internally.

| Platform | Resolved address |
|---|---|
| Linux / macOS | `/tmp/arc-ipc-<channel-id>` |
| Windows | `\\.\pipe\arc-ipc-<channel-id>` |

---

## Message Types

libipc carries two message types. Both share the same framing — only the
payload interpretation differs.

| Type | Description |
|---|---|
| `JSON` | UTF-8 encoded JSON object |
| `Binary` | Raw bytes, arbitrary length |

---

## Wire Format

Every message is prefixed by a fixed-size frame header followed immediately
by the payload bytes.

```
┌─────────────────────────────────────────────────────┐
│ magic       u32   0x41524349 ("ARCI")               │
│ version     u8    1                                  │
│ type        u8    0 = JSON, 1 = Binary               │
│ reserved    u16   0                                  │
│ payload_len u32   byte length of payload             │
└─────────────────────────────────────────────────────┘
[ payload bytes ... ]
```

- All multi-byte integers are little-endian
- `magic` is validated on every read — malformed frames close the channel
- `payload_len` is capped at 64 MiB — oversized frames close the channel
- JSON payloads are not null-terminated

---

## Public API

### ipc::Server

The listener side. libhost constructs one of these. Owns the platform socket
or named pipe and accepts exactly one client connection — arc operates on a
single controller per host instance.

```cpp
#include <ipc/ipc.hpp>

ipc::Server server("my-channel-234234");

server.on_connect([&] {
    // client connected, safe to send
});

server.on_disconnect([&] {
    // client disconnected or channel closed
});

server.on_message([&](ipc::Message msg) {
    if (msg.type == ipc::MessageType::JSON) {
        auto body = msg.json();
        // handle command
    }
});

server.on_error([&](ipc::Error err) {
    // transport or framing error
});

server.listen(); // non-blocking, begins accepting on background thread
server.stop();   // graceful shutdown
```

### ipc::Client

The connector side. Go uses this (via cgo binding or the Go libipc wrapper)
to connect to a running server.

```cpp
ipc::Client client("my-channel-234234");

client.on_connect([&] {
    // connected to server
});

client.on_disconnect([&] {
    // server disconnected or channel closed
});

client.on_message([&](ipc::Message msg) {
    if (msg.type == ipc::MessageType::JSON) {
        auto body = msg.json();
        // handle event
    }
});

client.on_error([&](ipc::Error err) {
    // transport or framing error
});

client.connect(); // non-blocking, connects on background thread
client.stop();    // graceful shutdown
```

### Sending Messages

Both `ipc::Server` and `ipc::Client` expose the same send interface.

```cpp
// send a JSON message
nlohmann::json body = { {"type", "host.ready"} };
server.send(body);

// send raw binary
std::vector<uint8_t> data = { 0x01, 0x02, 0x03 };
server.send(data);
```

---

## ipc::Message

Received messages are delivered as `ipc::Message` to the `on_message`
callback on both server and client.

```cpp
struct Message {
    MessageType type;   // JSON or Binary

    // valid when type == JSON
    nlohmann::json json() const;

    // valid when type == Binary
    std::span<const uint8_t> binary() const;
};
```

---

## ipc::MessageType

```cpp
enum class MessageType : uint8_t {
    JSON   = 0,
    Binary = 1,
};
```

---

## ipc::Error

```cpp
struct Error {
    ErrorCode   code;
    std::string message;
};

enum class ErrorCode {
    ConnectionRefused,   // client could not connect
    ConnectionLost,      // connection dropped mid-session
    BadFrame,            // magic mismatch or malformed header
    PayloadTooLarge,     // payload_len exceeded 64 MiB cap
    SendFailed,          // write to transport failed
    ListenFailed,        // server could not bind
};
```

---

## In-Process Mode

When Go runs as a `.so` inside the same process as libhost, libipc uses a
lockless in-process channel instead of a socket or pipe. The API is identical
— the transport is selected automatically based on whether the channel ID has
an active in-process registration.

```cpp
// before loading the Go .so, libhost registers an in-process channel
ipc::register_inprocess("my-channel-234234");

// Server constructs as normal — detects in-process registration,
// uses shared queue instead of socket
ipc::Server server("my-channel-234234");
```

From Go's side the client connects with the same channel ID and gets the
in-process transport automatically. No sockets, no pipes, no syscall overhead.

---

## Threading Model

libipc is internally threaded. Both `Server` and `Client` manage their own
background thread for I/O. All callbacks (`on_connect`, `on_message`,
`on_disconnect`, `on_error`) are dispatched on that background thread.

libhost is responsible for marshalling any callback work back onto the native
event loop thread if it needs to touch UI objects. libipc makes no assumptions
about which thread UI calls are safe on.

---

## Internal Components

```
ipc::Server / ipc::Client
└── ipc::Channel          (shared send/receive logic, framing, callbacks)
    └── ipc::Transport    (platform abstraction — socket or pipe or in-process)
        ├── UnixTransport     (Linux / macOS)
        ├── PipeTransport     (Windows)
        └── InProcessTransport (single binary mode)
```

### ipc::Transport (internal)

Pure virtual interface satisfied by each platform backend.

```cpp
class Transport {
public:
    virtual ~Transport() = default;
    virtual void     write(std::span<const uint8_t> data) = 0;
    virtual void     close() = 0;
    virtual bool     is_connected() const = 0;
};
```

### ipc::Channel (internal)

Owns the transport. Runs the read loop on a background thread. Handles frame
assembly, validates magic and length, dispatches parsed messages to registered
callbacks.

---

## Requirements

- C++20
- CMake 3.21+
- [nlohmann/json](https://github.com/nlohmann/json) (consumed via `find_package`)

**Linux / macOS:** GCC or Clang, POSIX sockets
**Windows:** MSVC, Win32 named pipes (`CreateNamedPipe` / `ConnectNamedPipe`)

---

## Building

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build
```

Headers installed under `include/ipc/`. Links as a static archive.

### Linking in Your Project

```cmake
find_package(libipc CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE ipc)
```

---

## Constraints and Non-Goals

- libipc accepts exactly one client per server instance — arc is a single
  controller model
- libipc does not provide request/reply correlation — that is the protocol
  layer's concern (libhost command/event model)
- libipc does not encrypt or authenticate — it assumes a trusted local channel
- libipc does not reconnect automatically — a disconnect is a terminal event,
  the application decides what to do
- libipc does not serialize application objects — it delivers raw JSON and
  binary, the caller interprets them