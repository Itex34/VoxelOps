#Shared/CMakeLists.txt



add_library(Shared STATIC
    network/Packets.cpp
    player/PlayerData.cpp
    player/HitboxCache.cpp
    player/MeshHitCache.cpp
    runtime/Paths.cpp
)

target_include_directories(Shared PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}   # so headers are found as "Shared/network/Packets.hpp"
)


target_compile_features(Shared PUBLIC cxx_std_23)
