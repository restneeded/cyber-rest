# cyber.rest — top-level CMake for the RED4ext client plugin (CyberRest / cr).
# Phase 0 skeleton: RED4ext.SDK + red-lib (git submodules) + GameNetworkingSockets (vcpkg).
# No protocol lib, no serialization lib — just enough to compile the host/join wiring.

cmake_minimum_required(VERSION 3.21)

project(CyberRest LANGUAGES CXX)

# Allow folders/filters in IDEs (e.g. Visual Studio).
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# C++20, matching the RED4ext.SDK / red-lib requirement.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED YES)

# --- RED4ext.SDK (git submodule at deps/RED4ext.SDK) ---
add_subdirectory(deps/RED4ext.SDK)
set_target_properties(RED4ext.SDK PROPERTIES FOLDER "Dependencies")
mark_as_advanced(
    RED4EXT_BUILD_EXAMPLES
    RED4EXT_HEADER_ONLY
)

# NOMINMAX must be defined before red-lib / <windows.h> is pulled in, otherwise the
# min/max macros collide with std::min/std::max used inside red-lib and GNS.
add_compile_definitions(NOMINMAX)

# --- psiberx red-lib (git submodule at deps/red-lib) — provides the RedLib target ---
add_subdirectory(deps/red-lib)

# --- GameNetworkingSockets from vcpkg (x64-windows triplet) ---
find_package(GameNetworkingSockets CONFIG REQUIRED)

# --- Our plugin sources ---
add_subdirectory(src)
