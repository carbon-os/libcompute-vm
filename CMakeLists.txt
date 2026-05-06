cmake_minimum_required(VERSION 3.25)
project(libcompute-vm VERSION 0.1.0 LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT APPLE)
    message(FATAL_ERROR "libcompute-vm currently only supports macOS")
endif()

set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0")

find_package(nlohmann_json CONFIG REQUIRED)

add_subdirectory(ipc)

add_library(compute-vm
    # Core Dispatcher
    src/vm.cpp

    # Apple Virtualization.framework Backend (VZ)
    src/vz/vz_backend.mm

    # Custom Hypervisor.framework Backend (HVF)
    src/hvf/hvf_backend.cpp
    src/hvf/vm_context.cpp
    src/hvf/memory.cpp
    src/hvf/vcpu.cpp
    src/hvf/exit_handler.cpp
    src/hvf/fw_dtb.cpp
    src/hvf/fw_gic.cpp
    src/hvf/fw_psci.cpp
    src/hvf/fw_loader.cpp
    src/hvf/virtio_block.cpp
    src/hvf/virtio_console.cpp
    src/hvf/virtio_gpu.cpp
    src/hvf/virtio_input.cpp
    src/hvf/virtio_net.cpp
    src/hvf/network.cpp
)

# Objective-C++ specific flags for the VZ backend
set_source_files_properties(src/vz/vz_backend.mm
    PROPERTIES COMPILE_FLAGS "-fobjc-arc"
)

target_include_directories(compute-vm
    PUBLIC  include
    PRIVATE src
)

target_link_libraries(compute-vm
    PRIVATE
        ipc
        nlohmann_json::nlohmann_json
        "-framework Virtualization"
        "-framework Hypervisor"
        "-framework Foundation"
)

add_subdirectory(cli)