cmake_minimum_required(VERSION 3.15)

project(VoxelOps LANGUAGES C CXX)

if(MSVC)
    if(POLICY CMP0141)
        cmake_policy(SET CMP0141 NEW)

        set(
            CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
            "$<IF:$<CONFIG:Debug,RelWithDebInfo>,ProgramDatabase,>"
        )
    endif()

    set(
        CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    )
endif()

add_subdirectory(VoxelOps)
add_subdirectory(VoxelOps-Headless)
add_subdirectory(Shared)

include(CheckIPOSupported)

check_ipo_supported(
    RESULT ipo_supported
    OUTPUT ipo_error
)

if(ipo_supported)
    set_property(TARGET VoxelOps
        PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

    set_property(TARGET VoxelOps-Headless
        PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()
