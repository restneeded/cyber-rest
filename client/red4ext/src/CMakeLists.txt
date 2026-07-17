# cyber.rest — plugin shared-library target (produces CyberRest.dll).

add_library(${PROJECT_NAME} SHARED "")

set(CR_HEADER_FILES
    Main.hpp
    PluginContext.hpp
    Protocol.hpp
    NetCore.hpp
    CyberRestSystem.hpp
    Identity.hpp
    HostStore.hpp
)

set(CR_SOURCE_FILES
    Main.cpp
    NetCore.cpp
    CyberRestSystem.cpp
    Identity.cpp
    HostStore.cpp
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${CR_HEADER_FILES} ${CR_SOURCE_FILES})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_sources(${PROJECT_NAME} PRIVATE ${CR_HEADER_FILES} ${CR_SOURCE_FILES})

target_link_libraries(${PROJECT_NAME} PRIVATE
    RED4ext::SDK
    RedLib
    GameNetworkingSockets::shared
)

# The RED4ext loader expects the plugin DLL to keep a stable name regardless of the
# CMake project/target name, so we pin the output name explicitly.
set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "CyberRest"
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
)
