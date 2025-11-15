cmake_minimum_required(VERSION 3.15)
project(ethercat_servo_control LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Add executable
add_executable(ethercat_servo_control WIN32
    src/main.c
)

# Add Windows-specific definitions
if(WIN32)
    target_compile_definitions(ethercat_servo_control PRIVATE WIN32_LEAN_AND_MEAN)
endif()

# Link Windows libraries
if(WIN32)
    target_link_libraries(ethercat_servo_control
        comctl32
        winmm
    )
endif()

# Set build type to Release by default
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler-specific options
if(MSVC)
    target_compile_options(ethercat_servo_control PRIVATE /W3)
    # Set subsystem to Windows to avoid console window
    set_target_properties(ethercat_servo_control PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:WINDOWS")
    # Add UNICODE and _UNICODE definitions to ensure proper character handling
    target_compile_definitions(ethercat_servo_control PRIVATE UNICODE _UNICODE)
else()
    target_compile_options(ethercat_servo_control PRIVATE -Wall -Wextra)
endif()

# Copy executable to project root after build for easier access
add_custom_command(TARGET ethercat_servo_control POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:ethercat_servo_control>
    ${CMAKE_SOURCE_DIR}/$<TARGET_FILE_NAME:ethercat_servo_control>
)