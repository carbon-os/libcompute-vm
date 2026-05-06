// src/transport.hpp

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace ipc::internal {

// Synchronous stream transport. read() blocks until buf is filled or the
// connection is lost. Returns 0 to signal disconnect.
class Transport {
public:
    virtual ~Transport() = default;

    virtual void   write(std::span<const uint8_t> data) = 0;
    virtual size_t read(std::span<uint8_t> buf)         = 0;
    virtual void   close()                              = 0;
    virtual bool   is_connected() const                 = 0;
};

} // namespace ipc::internal