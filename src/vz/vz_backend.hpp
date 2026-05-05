#pragma once

#include "backend.hpp"

#include <mutex>
#include <unordered_map>

namespace compute::vm::internal {

class VzBackend final : public Backend {
public:
    uint64_t Create(const nlohmann::json& config)       override;
    void     UI(uint64_t handle, void* native_handle)   override;
    void     Destroy(uint64_t handle)                   override;

private:
    std::mutex                          mutex_;
    std::unordered_map<uint64_t, void*> vms_;      // handle → VZVirtualMachine*
    uint64_t                            next_id_{1};
};

} // namespace compute::vm::internal