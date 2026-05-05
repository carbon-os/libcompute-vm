add_executable(vm-cli main.cpp)
target_link_libraries(vm-cli PRIVATE compute-vm nlohmann_json::nlohmann_json)