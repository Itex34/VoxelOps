cmake_minimum_required(VERSION 3.28 FATAL_ERROR)

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
    "world/ChunkManager.cpp"
    "world/WorldGen.cpp"
     
    "physics/Raycast.cpp" 
    "physics/HitDetectionSystem.cpp"
    
    "network/ServerNetwork.cpp"
    "network/ServerNetworkBlockEdits.cpp"
    "network/ServerNetworkGameplay.cpp"
    "network/ServerNetworkLoop.cpp"
    "network/WorldItemPhysics.cpp"
    "network/ServerNetworkChunkPipeline.cpp"
    "network/ServerNetworkCallbacks.cpp"
    "network/ServerNetworkPersistence.cpp"
    "network/CompressChunk.cpp"
    "network/ChunkStore.cpp"  
    "network/PacketReader.cpp"
    "network/PacketValidation.cpp"
    "network/PacketParsers.cpp"
    "network/ValidationSystem.cpp"
    "network/CombatRules.cpp"
    "network/ServerDiagnosticsFlags.cpp"
    "network/ServerNetworkConnection.cpp"
    "network/ServerNetworkCombat.cpp"
    "network/ServerNetworkChunkStreaming.cpp"
    "physics/RayManager.cpp" 
    "player/Hitbox.cpp" 
    "gun/Gun.cpp"
    "player/PlayerManager.cpp"
    "player/PlayerLifecycle.cpp"
    "player/PlayerUpdateSystem.cpp"
    "player/inventory/PlayerInventorySystem.cpp"
    "gameplay/combat/PlayerCombatSystem.cpp"
    "gameplay/combat/LagCompensationSystem.cpp"
    "gameplay/combat/ShootValidationSystem.cpp"
    "gameplay/combat/DamageSystem.cpp"
    "gameplay/combat/CombatFeedbackSystem.cpp"
    "gameplay/combat/ShootSystem.cpp"
    
    "network/snapshots/PlayerSnapshotSerializer.cpp" 
    "network/snapshots/PlayerSnapshotSystem.cpp"
    "player/ServerMovementSimulation.cpp"
    "network/PlayerInputBuffer.cpp"
    "world/WorldRaycast.cpp"
    "gameplay/spawn/Respawning.cpp")

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
