// src/transport_win.cpp

#if defined(_WIN32)

#include "transport_win.hpp"

#include <stdexcept>
#include <string>

namespace ipc::internal {

// ─── Pipe name ────────────────────────────────────────────────────────────────

std::string win_pipe_name(const std::string& channel_id) {
    return "\\\\.\\pipe\\arc-ipc-" + channel_id;
}

// ─── WinTransport ─────────────────────────────────────────────────────────────

WinTransport::WinTransport(HANDLE pipe) : pipe_(pipe) {}

WinTransport::~WinTransport() {
    close();
}

void WinTransport::write(std::span<const uint8_t> data) {
    DWORD total = 0;
    while (total < static_cast<DWORD>(data.size())) {
        DWORD written = 0;
        BOOL  ok      = WriteFile(pipe_,
                                  data.data() + total,
                                  static_cast<DWORD>(data.size()) - total,
                                  &written,
                                  nullptr);
        if (!ok || written == 0) {
            connected_ = false;
            return;
        }
        total += written;
    }
}

size_t WinTransport::read(std::span<uint8_t> buf) {
    DWORD total = 0;
    while (total < static_cast<DWORD>(buf.size())) {
        DWORD read_bytes = 0;
        BOOL  ok         = ReadFile(pipe_,
                                    buf.data() + total,
                                    static_cast<DWORD>(buf.size()) - total,
                                    &read_bytes,
                                    nullptr);
        if (!ok || read_bytes == 0) {
            connected_ = false;
            return 0;
        }
        total += read_bytes;
    }
    return static_cast<size_t>(total);
}

void WinTransport::close() {
    if (connected_.exchange(false)) {
        FlushFileBuffers(pipe_);
        DisconnectNamedPipe(pipe_);
        CloseHandle(pipe_);
    }
}

bool WinTransport::is_connected() const {
    return connected_;
}

// ─── Listen ───────────────────────────────────────────────────────────────────

std::unique_ptr<WinTransport> win_listen(const std::string& channel_id) {
    std::string name = win_pipe_name(channel_id);

    HANDLE pipe = CreateNamedPipeA(
        name.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,          // max instances — single controller
        65536,
        65536,
        0,
        nullptr
    );

    if (pipe == INVALID_HANDLE_VALUE)
        throw std::runtime_error("ipc: CreateNamedPipe failed: " + std::to_string(GetLastError()));

    BOOL ok = ConnectNamedPipe(pipe, nullptr); // blocks until client connects
    if (!ok && GetLastError() != ERROR_PIPE_CONNECTED) {
        CloseHandle(pipe);
        throw std::runtime_error("ipc: ConnectNamedPipe failed: " + std::to_string(GetLastError()));
    }

    return std::make_unique<WinTransport>(pipe);
}

// ─── Connect ──────────────────────────────────────────────────────────────────

std::unique_ptr<WinTransport> win_connect(const std::string& channel_id) {
    std::string name = win_pipe_name(channel_id);

    WaitNamedPipeA(name.c_str(), NMPWAIT_WAIT_FOREVER);

    HANDLE pipe = CreateFileA(
        name.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
    );

    if (pipe == INVALID_HANDLE_VALUE)
        throw std::runtime_error("ipc: CreateFile (pipe) failed: " + std::to_string(GetLastError()));

    return std::make_unique<WinTransport>(pipe);
}

} // namespace ipc::internal

#endif // _WIN32