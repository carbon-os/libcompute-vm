#include "compute/vm.hpp"
#include "backend.hpp"
#include "vz/vz_backend.hpp"
#include "hvf/hvf_backend.hpp"

#include <nlohmann/json.hpp>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <stdexcept>

namespace compute::vm {

namespace {
    std::mutex g_mtx;
    std::unordered_map<Handle, std::shared_ptr<internal::Backend>> g_backends;

    // We keep one instance of each factory to handle Create()
    internal::VzBackend  g_vz_factory;
    internal::hvf::HvfBackend g_hvf_factory;
}

Handle Create(const char* config_json) {
    auto config = nlohmann::json::parse(config_json);
    
    // Default to 'vz' if no engine is specified, or let user pick 'hvf'
    std::string engine = config.value("engine", "vz");
    
    Handle handle = 0;
    std::shared_ptr<internal::Backend> backend_ptr;

    if (engine == "hvf") {
        handle = g_hvf_factory.Create(config);
        backend_ptr = std::shared_ptr<internal::Backend>(&g_hvf_factory, [](auto*){}); // non-owning
    } else if (engine == "vz") {
        handle = g_vz_factory.Create(config);
        backend_ptr = std::shared_ptr<internal::Backend>(&g_vz_factory, [](auto*){}); // non-owning
    } else {
        throw std::invalid_argument("Unknown virtualization engine: " + engine);
    }

    std::lock_guard<std::mutex> lock(g_mtx);
    g_backends[handle] = backend_ptr;
    
    return handle;
}

void UI(Handle vm, void* native_handle) {
    std::shared_ptr<internal::Backend> backend;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        auto it = g_backends.find(vm);
        if (it != g_backends.end()) backend = it->second;
    }
    
    if (backend) backend->UI(vm, native_handle);
}

void Destroy(Handle vm) {
    std::shared_ptr<internal::Backend> backend;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        auto it = g_backends.find(vm);
        if (it != g_backends.end()) {
            backend = it->second;
            g_backends.erase(it);
        }
    }
    
    if (backend) backend->Destroy(vm);
}

} // namespace compute::vm