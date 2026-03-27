cmake_minimum_required(VERSION 3.28 FATAL_ERROR)

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
    "application/AppWindow.cpp"
    "application/AppSetup.cpp"
    "application/AppUi.cpp"
    "application/WorldItemRenderer.cpp"
    "application/AppSimulation.cpp"
    "application/AppLifecycle.cpp"
    "application/AppHelpers.cpp"
    "graphics/Lighting.cpp"
    "network/ClientNetwork.cpp"
    "network/DecompressChunk.cpp"
    "runtime/ClientReconciler.cpp"
    "runtime/SnapshotInterpolator.cpp"
    "player/Hitbox.cpp"    
    "gun/Gun.cpp"
    "misc/ThreadPool.cpp" 
    "graphics/RegionMeshBuffer.cpp"
    "voxels/ChunkColumn.cpp"
    "graphics/WorldGen.cpp"
    "graphics/Backend.cpp"
    "graphics/Sky.cpp" 
    "graphics/PostProcessor.cpp"
    "ui/debug/DebugUi.cpp" "ui/player/InventoryUI.cpp" "ui/player/Crosshair.cpp" "commands/CommandParser.cpp" "ui/player/SettingsUI.cpp" "ui/player/MainMenu.cpp")

find_package(glfw3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(assimp CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(GameNetworkingSockets CONFIG REQUIRED)
find_package(lz4 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

target_link_libraries(VoxelOps PRIVATE glfw)
target_link_libraries(VoxelOps PRIVATE glm::glm)
target_link_libraries(VoxelOps PRIVATE assimp::assimp)
target_link_libraries(VoxelOps PRIVATE glad::glad)
target_link_libraries(VoxelOps PRIVATE GameNetworkingSockets::shared GameNetworkingSockets::static GameNetworkingSockets::GameNetworkingSockets GameNetworkingSockets::GameNetworkingSockets_s)
target_link_libraries(VoxelOps PRIVATE Shared)
target_link_libraries(VoxelOps PRIVATE lz4::lz4)
target_link_libraries(VoxelOps PRIVATE imgui::imgui)

target_include_directories(VoxelOps PRIVATE ${Stb_INCLUDE_DIR})
target_include_directories(VoxelOps PRIVATE ../Shared)
target_include_directories(VoxelOps PRIVATE ../third_party/precomputed_atmospheric_scattering)




set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT "VoxelOps")
set_property(TARGET VoxelOps PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
set_property(TARGET VoxelOps PROPERTY INTERPROCEDURAL_OPTIMIZATION_DISTRIBUTION TRUE)

set(CMAKE_CONFIGURATION_TYPES "Debug;Release;Distribution" CACHE STRING "" FORCE)

if(MSVC)
    target_compile_options(VoxelOps PRIVATE /GR- /EHs-)
else()
    target_compile_options(VoxelOps PRIVATE -fno-rtti -fno-exceptions)
endif()
