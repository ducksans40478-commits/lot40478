cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

# Only force universal (Intel + Apple Silicon) architectures for plain
# macOS builds. iOS builds set their own architectures/sysroots via the
# build-geode-mod action, and forcing this here conflicts with that.
if (NOT DEFINED CMAKE_SYSTEM_NAME AND NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()

project(InputForgivenessSuite VERSION 1.0.0)

add_library(${PROJECT_NAME} SHARED
    src/main.cpp
)

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK.")
else()
    message(STATUS "Found Geode SDK: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

setup_geode_mod(${PROJECT_NAME})
