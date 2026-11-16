cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME steeringwheel_firmware)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
# NOTE: Core/Src/centering_law.c is intentionally NOT listed here. It is pure
# math, unit-tested on the host only, and must not be wired to the ODrive
# before the belt, encoder calibration, tightened ODrive limits, watchdog,
# e-stop and the owner's explicit approval (docs/DECISIONS.md).
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    Core/Src/encoder.c
    Core/Src/buttons_mcp23017.c
    Core/Src/steering_pot.c
    Core/Src/hid_report.c
    Core/Src/hid_pack.c
    Core/Src/odrive_ascii.c
    Core/Src/odrive_link.c
    Core/Src/odrive_uart.c
    Core/Src/steering_map.c
    Core/Src/debounce.c
    Core/Src/mcp23017_core.c
    Core/Src/axis_shape.c
    Core/Src/board_health.c
    Core/Src/settings.c
    Core/Src/settings_flash_stm32.c
)

# Be strict with our own sources (generated CubeMX code keeps default flags)
set_source_files_properties(
    Core/Src/encoder.c
    Core/Src/buttons_mcp23017.c
    Core/Src/steering_pot.c
    Core/Src/hid_report.c
    Core/Src/hid_pack.c
    Core/Src/odrive_ascii.c
    Core/Src/odrive_link.c
    Core/Src/odrive_uart.c
    Core/Src/steering_map.c
    Core/Src/debounce.c
    Core/Src/mcp23017_core.c
    Core/Src/axis_shape.c
    Core/Src/board_health.c
    Core/Src/settings.c
    Core/Src/settings_flash_stm32.c
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

# Force-feedback sources: compiled ONLY with -DWHEEL_FFB=ON (see below).
# Everything that can write to the ODrive lives in Core/Src/ffb.
set(WHEEL_FFB_SOURCES
    Core/Src/ffb/ffb_board.c
    Core/Src/ffb/ffb_cmd.c
    Core/Src/ffb/ffb_effects.c
    Core/Src/ffb/ffb_hid_desc.c
    Core/Src/ffb/ffb_pid.c
    Core/Src/ffb/ffb_precheck.c
    Core/Src/ffb/ffb_safety.c
    Core/Src/ffb/ffb_service.c
    Core/Src/ffb/ffb_torque.c
    Core/Src/ffb/ffb_ui.c
    Core/Src/ffb/usbd_ffb_hid.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
)

# Add project symbols (macros)
# Build options (see Core/Inc/wheel_config.h for all tunables):
#   -DSTEERING_SOURCE=ODRIVE|ENCODER|POT
#                                   steering input (default ODRIVE: position
#                                   read over USART1 from the ODrive;
#                                   ENCODER = encoder on STM32 TIM3, POT = PA1)
#   -DSIMULATE_INPUTS=ON            sine on X + walking buttons, no wiring needed
set(STEERING_SOURCE "ODRIVE" CACHE STRING "Steering input: ODRIVE, ENCODER or POT")
set_property(CACHE STEERING_SOURCE PROPERTY STRINGS ODRIVE ENCODER POT)
option(SIMULATE_INPUTS "Generate simulated inputs instead of reading hardware" OFF)

#   -DWHEEL_FFB=ON                  force feedback (Core/Src/ffb). Default OFF:
#                                   the firmware is then read-only towards the
#                                   ODrive. See docs/FFB_SAFETY.md.
#   -DWHEEL_FFB_ARM_ALLOWED=1       with WHEEL_FFB=ON: allow the motor to be
#                                   armed at all (default 0 = never arms).
option(WHEEL_FFB "Build the force-feedback module (can command the motor)" OFF)
set(WHEEL_FFB_ARM_ALLOWED "0" CACHE STRING "1 = FFB may arm the motor (needs WHEEL_FFB=ON)")
set_property(CACHE WHEEL_FFB_ARM_ALLOWED PROPERTY STRINGS 0 1)

#   -DWHEEL_EXTRA_DEFINES="A=1;B=2"  extra macros, e.g. the optional features
#                                   IWDG_ENABLE, RESET_REASON_LED, AXIS_SHAPING_ENABLE,
#                                   SETTINGS_FLASH_ENABLE, ODRIVE_RECENTRE_AFTER_STALE_MS
set(WHEEL_EXTRA_DEFINES "" CACHE STRING "Extra compile definitions (semicolon list)")

set(WHEEL_DEFINES STEERING_SOURCE=STEERING_SOURCE_${STEERING_SOURCE} ${WHEEL_EXTRA_DEFINES})
if(SIMULATE_INPUTS)
    list(APPEND WHEEL_DEFINES SIMULATE_INPUTS=1)
endif()
if(WHEEL_FFB)
    list(APPEND WHEEL_DEFINES WHEEL_FFB=1 WHEEL_FFB_ARM_ALLOWED=${WHEEL_FFB_ARM_ALLOWED})
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${WHEEL_FFB_SOURCES})
    set_source_files_properties(${WHEEL_FFB_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")
    target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE Core/Inc/ffb)
    message(STATUS "WHEEL_FFB=ON (WHEEL_FFB_ARM_ALLOWED=${WHEEL_FFB_ARM_ALLOWED})")
elseif(NOT WHEEL_FFB_ARM_ALLOWED STREQUAL "0")
    message(FATAL_ERROR "WHEEL_FFB_ARM_ALLOWED needs WHEEL_FFB=ON")
endif()

# Hardware profile -> Core/Inc/wheel_config_generated.h. The generated file is
# committed, so a build without Python still works; with Python 3.11+ it is
# regenerated whenever config/hardware_profile.toml changes. An invalid
# profile fails the build on purpose.
find_package(Python3 3.11 COMPONENTS Interpreter QUIET)
set(WHEEL_PROFILE   ${CMAKE_CURRENT_SOURCE_DIR}/../config/hardware_profile.toml)
set(WHEEL_GEN       ${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_config.py)
set(WHEEL_GEN_HDR   ${CMAKE_CURRENT_SOURCE_DIR}/Core/Inc/wheel_config_generated.h)
if(Python3_Interpreter_FOUND AND EXISTS ${WHEEL_PROFILE})
    add_custom_command(
        OUTPUT  ${WHEEL_GEN_HDR}
        COMMAND ${Python3_EXECUTABLE} ${WHEEL_GEN} --header-only --quiet
        DEPENDS ${WHEEL_PROFILE} ${WHEEL_GEN} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/odrive_enums.py
        COMMENT "Hardware profile -> wheel_config_generated.h"
        VERBATIM)
    add_custom_target(wheel_profile DEPENDS ${WHEEL_GEN_HDR})
    add_dependencies(${CMAKE_PROJECT_NAME} wheel_profile)
else()
    message(STATUS "Python 3.11+ not found: using the committed wheel_config_generated.h")
endif()

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
    ${WHEEL_DEFINES}
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
