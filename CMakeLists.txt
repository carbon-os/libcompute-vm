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
    src/vm.cpp
    src/vz/vz_backend.mm
)

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
        "-framework Foundation"
)

add_subdirectory(cli)