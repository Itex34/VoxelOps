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
     
    "physics/HitDetectionSystem.cpp"
    
    "network/ServerNetwork.cpp"
    "network/core/ServerRuntime.cpp"
    "network/core/ServerRuntimeConnection.cpp"
    "network/core/ServerRuntimeLoop.cpp"
    "network/core/ServerRuntimeSessionLifecycle.cpp"
    "network/core/ServerRuntimeCallbacks.cpp"
    "network/core/ServerRuntimePersistence.cpp"
    "network/core/ServerDiagnosticsFlags.cpp"
    "network/protocol/PacketReader.cpp"
    "network/protocol/PacketValidation.cpp"
    "network/protocol/PacketParsers.cpp"
    "network/protocol/ValidationSystem.cpp"
    "network/gameplay/ServerNetworkBlockEdits.cpp"
    "network/gameplay/ServerNetworkGameplay.cpp"
    "network/gameplay/ServerNetworkCombat.cpp"
    "network/gameplay/WorldItemPhysics.cpp"
    "network/gameplay/CombatRules.cpp"
    "network/gameplay/PlayerInputBuffer.cpp"
    "network/replication/ServerNetworkChunkPipeline.cpp"
    "network/replication/ServerNetworkChunkStreaming.cpp"
    "network/replication/CompressChunk.cpp"
    "network/replication/ChunkStore.cpp"
    "player/Hitbox.cpp" 
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
