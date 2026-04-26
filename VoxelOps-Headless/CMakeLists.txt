cmake_minimum_required(VERSION 3.28 FATAL_ERROR)

project(VoxelOps-Headless CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

add_executable(VoxelOps-Headless 
    "server_main.cpp"  
   
    "engine/voxels/Voxel.cpp"
    "engine/voxels/ServerChunk.cpp"    
    "engine/world/ChunkManager.cpp"
    "engine/world/WorldGen.cpp"   
    "engine/physics/core/HitDetection.cpp"
    "engine/physics/simulation/WorldItemPhysics.cpp"
    "engine/world/WorldRaycast.cpp"

    "network/ServerNetwork.cpp"
    "network/core/ServerRuntime.cpp"
    "network/core/ServerRuntimeConnection.cpp"
    "network/core/tick/TickPipeline.cpp"
    "network/core/tick/TickNetwork.cpp"
    "network/core/tick/TickSimulation.cpp"
    "network/core/tick/TickDiagnostics.cpp"
    "network/core/tick/TickReplication.cpp"
    "network/core/tick/TickGameplay.cpp"
    "network/core/tick/TickWorld.cpp"
    "network/core/ServerRuntimeSessionLifecycle.cpp"
    "network/core/ServerRuntimeCallbacks.cpp"
    "network/core/ServerRuntimePersistence.cpp"
    "network/core/ServerDiagnosticsFlags.cpp"
    "network/protocol/PacketReader.cpp"
    "network/protocol/PacketValidation.cpp"
    "network/protocol/PacketParsers.cpp"
    "network/protocol/Validation.cpp"
    "/gameplay/BlockEditsNetwork.cpp"
    "network/gameplay/ServerNetworkGameplay.cpp"
    "network/gameplay/CombatNetwork.cpp"
    "network/gameplay/CombatRules.cpp"
    "network/gameplay/PlayerInputBuffer.cpp"
    "network/replication/ServerNetworkChunkPipeline.cpp"
    "network/replication/ServerNetworkChunkStreaming.cpp"
    "network/replication/CompressChunk.cpp"
    "network/replication/ChunkStore.cpp"
    "network/snapshots/PlayerSnapshotSerializer.cpp" 
    "network/snapshots/PlayerSnapshot.cpp"

    "game/player/Hitbox.cpp" 
    "game/player/PlayerManager.cpp"
    "game/player/PlayerLifecycle.cpp"
    "game/player/PlayerUpdate.cpp"
    "game/player/inventory/PlayerInventory.cpp"
    "game/combat/PlayerCombat.cpp"
    "game/combat/LagCompensation.cpp"
    "game/combat/ShootValidation.cpp"
    "game/combat/Damage.cpp"
    "game/combat/CombatFeedback.cpp"
    "game/combat/Shoot.cpp"
    "game/combat/Combat.cpp"
    "game/player/ServerPlayer.cpp"
    "game/player/ServerMovementSimulation.cpp"
    "game/world/BlockEdits.cpp"
    "game/spawn/Respawning.cpp")

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
