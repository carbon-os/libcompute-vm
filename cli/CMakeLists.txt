add_executable(vm-cli main.cpp)
target_link_libraries(vm-cli PRIVATE compute-vm ipc nlohmann_json::nlohmann_json)

if(APPLE)
    # Point directly to the source file instead of generating it
    set(ENTITLEMENTS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/vm-cli.entitlements")

    add_custom_command(TARGET vm-cli POST_BUILD
        COMMAND codesign
            --sign -
            --entitlements "${ENTITLEMENTS_FILE}"
            --force
            "$<TARGET_FILE:vm-cli>"
        COMMENT "Signing vm-cli with virtualization and hypervisor entitlements"
    )
endif()