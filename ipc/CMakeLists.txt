# CMakeLists.txt

cmake_minimum_required(VERSION 3.21)
project(libipc VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(nlohmann_json CONFIG REQUIRED)

set(IPC_SOURCES
    src/channel.cpp
    src/transport_inprocess.cpp
    src/server.cpp
    src/client.cpp
)

if(WIN32)
    list(APPEND IPC_SOURCES src/transport_win.cpp)
else()
    list(APPEND IPC_SOURCES src/transport_unix.cpp)
endif()

add_library(ipc STATIC ${IPC_SOURCES})

target_include_directories(ipc
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(ipc PUBLIC nlohmann_json::nlohmann_json)

install(TARGETS ipc EXPORT libipcTargets ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
install(EXPORT libipcTargets
    FILE        libipcConfig.cmake
    NAMESPACE   ipc::
    DESTINATION lib/cmake/libipc
)