add_executable(vm-cli main.cpp)
target_link_libraries(vm-cli PRIVATE compute-vm ipc nlohmann_json::nlohmann_json)

if(APPLE)
    set(ENTITLEMENTS_FILE "${CMAKE_CURRENT_BINARY_DIR}/vm-cli.entitlements")
    file(WRITE "${ENTITLEMENTS_FILE}"
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\"\n"
        "  \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\"><dict>\n"
        "  <key>com.apple.security.virtualization</key><true/>\n"
        "</dict></plist>\n"
    )

    add_custom_command(TARGET vm-cli POST_BUILD
        COMMAND codesign
            --sign -
            --entitlements "${ENTITLEMENTS_FILE}"
            --force
            "$<TARGET_FILE:vm-cli>"
        COMMENT "Signing vm-cli with virtualization entitlement"
    )
endif()