cmake_minimum_required(VERSION 3.28 FATAL_ERROR)

project(VoxelOps-Headless CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

add_executable(VoxelOps-Headless 
    "server_main.cpp"  
   
    "engine/voxels/Voxel.cpp"
    "engine/voxels/ServerChunk.cpp"    
    "engine/world/ChunkManager.cpp"
    "engine/world/WorldGen.cpp"   
    "engine/physics/core/HitDetection.cpp"
    "engine/physics/simulation/WorldItemPhysics.cpp"
    "engine/world/WorldRaycast.cpp"

    "network/core/ServerHost.cpp"
    "network/core/ServerComposition.cpp"
    "network/core/NetworkRuntime.cpp"
    "network/core/ServerControlService.cpp"
    "network/core/ServerTickLoop.cpp"
    "network/core/DiagnosticsFlags.cpp"

    "network/tick/TickNetworkPhase.cpp"
    "network/tick/NetworkPump.cpp"
    "network/tick/ReplicationPhase.cpp"
    "network/tick/ChunkInterestPhase.cpp"
    "network/tick/ChunkSendPhase.cpp"
    "network/tick/CollisionPrewarmPhase.cpp"
    "network/tick/GameplayPhase.cpp"
    "network/tick/DiagnosticsPhase.cpp"
    "network/tick/SimulationPhase.cpp"
    "network/connection/BroadcastService.cpp"
    "network/connection/ConnectionService.cpp"
    "network/chunk/ChunkPipelineState.cpp"
    "network/chunk/ChunkStreamingService.cpp"
    

    "network/session/ClientSessionManager.cpp"
    "network/session/SessionState.cpp"
    "network/session/SessionLifecycleService.cpp"
    "network/persistence/ChatService.cpp"
    "network/persistence/AdminService.cpp"
    "network/persistence/AdminControlService.cpp"
    "network/protocol/PacketReader.cpp"
    "network/protocol/PacketValidation.cpp"
    "network/protocol/PacketParsers.cpp"
    "network/protocol/Validation.cpp"
    "network/gameplay/WorldItemService.cpp"
    "network/gameplay/InventoryActionService.cpp"
    "network/gameplay/BlockEditExecutionService.cpp"
    "network/gameplay/CombatExecutionService.cpp"
    "network/gameplay/BlockEditRequestService.cpp"
    "network/gameplay/CombatRequestService.cpp"
    "network/gameplay/MatchService.cpp"
    "network/gameplay/Rules.cpp"
    "network/gameplay/InputBuffer.cpp"
    "network/replication/CompressChunk.cpp"
    "network/replication/ChunkStore.cpp"
    "network/snapshots/PlayerSnapshotSerializer.cpp" 
    "network/snapshots/PlayerSnapshots.cpp"

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
    "game/player/ServerPlayer.cpp"
    "game/player/ServerMovementSimulation.cpp"
    "game/spawn/Respawning.cpp" 
    "engine/world/WorldCollision.cpp" "game/combat/GrappleGun.cpp")

find_package(glm CONFIG REQUIRED)
find_package(GameNetworkingSockets CONFIG REQUIRED)
find_package(lz4 CONFIG REQUIRED)

target_link_libraries(VoxelOps-Headless PRIVATE glm::glm)
target_link_libraries(VoxelOps-Headless PRIVATE GameNetworkingSockets::GameNetworkingSockets)
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
