cmake_minimum_required(VERSION 3.20 FATAL_ERROR)

project(VoxelOps-Headless CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

add_executable(VoxelOps-Headless 
    "server_main.cpp"  
    
    
    "voxels/Voxel.cpp"
    "voxels/ServerChunk.cpp" 
    
    "player/ServerPlayer.cpp"
    "graphics/ChunkManager.cpp"
    "graphics/WorldGen.cpp"
     
    "physics/Raycast.cpp" 
    
    "network/ServerNetwork.cpp"
    "network/CompressChunk.cpp"
    "network/ChunkStore.cpp"  
    "physics/RayManager.cpp" 
    "player/Hitbox.cpp" 
    "gun/Gun.cpp"
    "player/PlayerManager.cpp"
    
)

find_package(glm CONFIG REQUIRED)
find_package(GameNetworkingSockets CONFIG REQUIRED)
find_package(lz4 CONFIG REQUIRED)

target_link_libraries(VoxelOps-Headless PRIVATE glm::glm)
target_link_libraries(VoxelOps-Headless PRIVATE GameNetworkingSockets::shared GameNetworkingSockets::static GameNetworkingSockets::GameNetworkingSockets GameNetworkingSockets::GameNetworkingSockets_s)
target_link_libraries(VoxelOps-Headless PRIVATE Shared)
target_link_libraries(VoxelOps-Headless PRIVATE lz4::lz4)



set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT "VoxelOps-Headless")
set_property(TARGET VoxelOps-Headless PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
set_property(TARGET VoxelOps-Headless PROPERTY INTERPROCEDURAL_OPTIMIZATION_DISTRIBUTION TRUE)

set(CMAKE_CONFIGURATION_TYPES "Debug;Release;Distribution" CACHE STRING "" FORCE)

if(MSVC)
    target_compile_options(VoxelOps-Headless PRIVATE /GR- /EHsc)
else()
    target_compile_options(VoxelOps-Headless PRIVATE -frtti -fexceptions)
endif()
