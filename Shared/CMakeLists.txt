#Shared/CMakeLists.txt

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

add_library(Shared STATIC
    "network/Packets.cpp"
    "player/PlayerData.cpp"
    "player/HitboxCache.cpp"
    "player/MeshHitCache.cpp"
    "runtime/Paths.cpp"
    "player/Inventory.cpp"
    "items/Items.cpp"
    "player/BlockPlace.cpp"
)

target_include_directories(Shared PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}   # so headers are found as "Shared/network/Packets.hpp"
)


#target_compile_features(Shared PUBLIC cxx_std_23)
