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
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    Core/Src/encoder.c
    Core/Src/buttons_mcp23017.c
    Core/Src/steering_pot.c
    Core/Src/hid_report.c
)

# Be strict with our own sources (generated CubeMX code keeps default flags)
set_source_files_properties(
    Core/Src/encoder.c
    Core/Src/buttons_mcp23017.c
    Core/Src/steering_pot.c
    Core/Src/hid_report.c
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
)

# Add project symbols (macros)
# Build options (see Core/Inc/wheel_config.h for all tunables):
#   -DSTEERING_SOURCE=ENCODER|POT   steering input (default ENCODER)
#   -DSIMULATE_INPUTS=ON            sine on X + walking buttons, no wiring needed
set(STEERING_SOURCE "ENCODER" CACHE STRING "Steering input: ENCODER or POT")
set_property(CACHE STEERING_SOURCE PROPERTY STRINGS ENCODER POT)
option(SIMULATE_INPUTS "Generate simulated inputs instead of reading hardware" OFF)

set(WHEEL_DEFINES STEERING_SOURCE=STEERING_SOURCE_${STEERING_SOURCE})
if(SIMULATE_INPUTS)
    list(APPEND WHEEL_DEFINES SIMULATE_INPUTS=1)
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
