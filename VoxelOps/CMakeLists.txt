cmake_minimum_required(VERSION 3.20 FATAL_ERROR)

project(VoxelOps CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")


add_executable(VoxelOps 
    "main.cpp"  
    "graphics/Camera.cpp" 
    "graphics/Shader.cpp" 
    "graphics/Model.cpp"
    "graphics/Mesh.cpp" 
    "graphics/ChunkMeshBuilder.cpp"
    "voxels/Voxel.cpp"
    "voxels/Chunk.cpp" 
    "graphics/Renderer.cpp"
    "physics/Physics.cpp" 
    "player/Player.cpp"
    "input/InputCallbacks.cpp"
    "data/GameData.hpp"
    "data/GameData.cpp" 
    "graphics/Frustum.hpp"
    "graphics/ChunkManager.cpp"
    "graphics/ChunkRenderSystem.cpp"
    "graphics/TextureAtlas.cpp" 
    "physics/Raycast.cpp" 
    "physics/RayManager.cpp"
    "application/VoxelopsApp.cpp"
    "graphics/Lighting.cpp"
    "network/ClientNetwork.cpp"
    "player/Hitbox.cpp"    
    "gun/Gun.cpp"
    "misc/ThreadPool.cpp" 
    "graphics/RegionMeshBuffer.cpp"
    "voxels/ChunkColumn.cpp"
    "graphics/WorldGen.cpp"
    "graphics/Sky.cpp"
    "../third_party/precomputed_atmospheric_scattering/atmosphere/model.cc"
)

find_package(glfw3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(assimp CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(unofficial-joltphysics CONFIG REQUIRED)
find_package(GameNetworkingSockets CONFIG REQUIRED)


target_link_libraries(VoxelOps PRIVATE glfw)
target_link_libraries(VoxelOps PRIVATE glm::glm)
target_link_libraries(VoxelOps PRIVATE assimp::assimp)
target_link_libraries(VoxelOps PRIVATE glad::glad)
target_link_libraries(VoxelOps PRIVATE unofficial::joltphysics::Jolt)
target_link_libraries(VoxelOps PRIVATE GameNetworkingSockets::shared GameNetworkingSockets::static GameNetworkingSockets::GameNetworkingSockets GameNetworkingSockets::GameNetworkingSockets_s)
target_link_libraries(VoxelOps PRIVATE Shared)


target_include_directories(VoxelOps PRIVATE ${Stb_INCLUDE_DIR})
target_include_directories(VoxelOps PRIVATE ../Shared)
target_include_directories(VoxelOps PRIVATE ../third_party/precomputed_atmospheric_scattering)


# include the headers!


target_compile_definitions(VoxelOps
    PRIVATE
        $<$<CONFIG:Debug>:JPH_PROFILE_ENABLED=0>
        $<$<CONFIG:Release>:JPH_DISABLE_PROFILING>
)

target_compile_definitions(VoxelOps
    PRIVATE
    $<$<CONFIG:Debug>:JPH_DEBUG_RENDERER>
)



set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT "VoxelOps")
set_property(TARGET VoxelOps PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
set_property(TARGET VoxelOps PROPERTY INTERPROCEDURAL_OPTIMIZATION_DISTRIBUTION TRUE)

set(CMAKE_CONFIGURATION_TYPES "Debug;Release;Distribution" CACHE STRING "" FORCE)

if(MSVC)
    target_compile_options(VoxelOps PRIVATE /GR- /EHs-)
else()
    target_compile_options(VoxelOps PRIVATE -fno-rtti -fno-exceptions)
endif()

message(STATUS "Jolt include dir: ${Jolt_INCLUDE_DIRS}")
message(STATUS "Jolt libraries: ${Jolt_LIBRARIES}")
