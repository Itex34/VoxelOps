cmake_minimum_required(VERSION 3.15)

project(VoxelOps LANGUAGES C CXX)

include(CTest)
if(BUILD_TESTING)
    add_executable(VoxelOpsRtResourceLifetimeTests VoxelOps/tests/RtResourceLifetimeTests.cpp)
    target_compile_features(VoxelOpsRtResourceLifetimeTests PRIVATE cxx_std_20)
    add_test(NAME VoxelOpsRtResourceLifetimeTests COMMAND VoxelOpsRtResourceLifetimeTests)
endif()

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

if(BUILD_TESTING)
    find_package(glm CONFIG REQUIRED)
    add_executable(VoxelOpsGiMaterialSolidityTests VoxelOps/tests/GiMaterialSolidityTests.cpp)
    target_compile_features(VoxelOpsGiMaterialSolidityTests PRIVATE cxx_std_20)
    target_link_libraries(VoxelOpsGiMaterialSolidityTests PRIVATE glm::glm)
    add_test(NAME VoxelOpsGiMaterialSolidityTests COMMAND VoxelOpsGiMaterialSolidityTests)
endif()

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
