#pragma once

#include <cstdint>

namespace compute::vm {

using Handle = uint64_t;

Handle Create(const char* config_json);
void   UI(Handle vm, void* native_handle);
void   Destroy(Handle vm);

} // namespace compute::vm