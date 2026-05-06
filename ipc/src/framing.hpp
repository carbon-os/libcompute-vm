// src/framing.hpp

#pragma once

#include <cstdint>

namespace ipc::internal {

constexpr uint32_t kMagic          = 0x41524349; // "ARCI"
constexpr uint8_t  kVersion        = 1;
constexpr uint32_t kMaxPayloadSize = 64u * 1024u * 1024u; // 64 MiB

#pragma pack(push, 1)
struct FrameHeader {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;       // ipc::MessageType
    uint16_t reserved;
    uint32_t payload_len;
};
#pragma pack(pop)

static_assert(sizeof(FrameHeader) == 12);

} // namespace ipc::internal