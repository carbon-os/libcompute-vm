#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>

namespace compute::vm::internal {

class Backend {
public:
    virtual ~Backend() = default;

    virtual uint64_t Create(const nlohmann::json& config)            = 0;
    virtual void     UI(uint64_t handle, void* native_handle)        = 0;
    virtual void     Destroy(uint64_t handle)                        = 0;
};

} // namespace compute::vm::internal