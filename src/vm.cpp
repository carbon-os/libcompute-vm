#include "compute/vm.hpp"
#include "backend.hpp"
#include "vz/vz_backend.hpp"

#include <nlohmann/json.hpp>
#include <memory>

namespace compute::vm {

static internal::Backend& backend() {
    static internal::VzBackend instance;
    return instance;
}

Handle Create(const char* config_json) {
    return backend().Create(nlohmann::json::parse(config_json));
}

void UI(Handle vm, void* native_handle) {
    backend().UI(vm, native_handle);
}

void Destroy(Handle vm) {
    backend().Destroy(vm);
}

} // namespace compute::vm