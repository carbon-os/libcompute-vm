#pragma once

#include "backend.hpp"
#include "hvf/vm_context.hpp"
#include <ipc/ipc.hpp>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace compute::vm::internal::hvf {

struct SerialChannel {
    int                          host_write_fd{ -1 }; // host → VM
    int                          host_read_fd{ -1 };  // VM → host
    std::unique_ptr<ipc::Server> server;
    std::jthread                 pump_thread;         // VM output → IPC
};

struct Instance {
    std::unique_ptr<VMContext> ctx;
    std::thread                run_thread;
    std::vector<SerialChannel> serial;
};

class HvfBackend final : public Backend {
public:
    uint64_t Create(const nlohmann::json& config)       override;
    void     UI(uint64_t handle, void* native_handle)   override;
    void     Destroy(uint64_t handle)                   override;

private:
    std::mutex                               mutex_;
    std::unordered_map<uint64_t, Instance>   vms_;
    uint64_t                                 next_id_{ 1 };
};

} // namespace compute::vm::internal::hvf