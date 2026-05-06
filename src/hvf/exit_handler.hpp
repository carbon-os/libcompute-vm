#pragma once

namespace compute::vm::internal::hvf {
class VCpu;

struct ExitHandler {
    // Returns false → stop this vCPU thread (unrecoverable fault or halt).
    [[nodiscard]] static bool handle(VCpu& vcpu);
};

} // namespace compute::vm::internal::hvf