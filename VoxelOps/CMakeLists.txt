cmake_minimum_required(VERSION 3.28 FATAL_ERROR)

project(VoxelOps CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

set(PBRSKY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../PbrSkyLibOpenGL")
if(NOT EXISTS "${PBRSKY_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "PbrSkyLibOpenGL not found at ${PBRSKY_ROOT}")
endif()



set(SKYATMOS_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
set(SKYATMOS_FETCH_DEPS OFF CACHE BOOL "" FORCE)
add_subdirectory("${PBRSKY_ROOT}" "${CMAKE_BINARY_DIR}/pbrsky")

option(VOXELOPS_ENABLE_NRD_BOOTSTRAP "Enable early NRD API bootstrap integration" ON)
set(VOXELOPS_NRD_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../NRD" CACHE PATH "Path to NVIDIA NRD repository")
set(VOXELOPS_NRD_DLL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../NRD-Sample/_Bin/NRD.dll" CACHE FILEPATH "Path to NRD.dll used by runtime bootstrap")

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
    "graphics/OpenGLRenderDevice.cpp"
    "graphics/VulkanRenderDevice.cpp"
    "graphics/RenderDeviceFactory.cpp"
    "graphics/Vulkan/renderer/FrameSync.cpp"
    "graphics/Vulkan/renderer/NrdBootstrap.cpp"
    "graphics/Vulkan/renderer/Pipeline.cpp"
    "graphics/Vulkan/renderer/RenderPass.cpp"
    "graphics/Vulkan/renderer/VulkanRenderer.cpp"
    "graphics/Vulkan/graphics/Mesh.cpp"
    "graphics/Vulkan/graphics/Model.cpp"
    "graphics/Vulkan/graphics/StbImageImpl.cpp"
    "graphics/Vulkan/graphics/VkTexture.cpp"
    "graphics/Vulkan/vulkan/UploadContext.cpp"
    "graphics/Vulkan/vulkan/VulkanContext.cpp"
    "graphics/Vulkan/vulkan/VulkanUtils.cpp"
     
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
    "graphics/ISkyBackend.hpp"
    "graphics/RealisticSkyBackend.hpp"
    "graphics/RealisticSkyBackend.cpp"
    "graphics/ShaderSkyBackend.hpp"
    "graphics/ShaderSkyBackend.cpp"
    "graphics/PostProcessor.cpp"
    "ui/debug/DebugUi.cpp"
    "ui/player/InventoryUI.cpp"
    "ui/player/Crosshair.cpp"
    "commands/CommandParser.cpp"
    "ui/player/SettingsUI.cpp"
    "ui/player/MainMenu.cpp"
)

find_package(SDL3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(assimp CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(GameNetworkingSockets CONFIG REQUIRED)
find_package(lz4 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(Vulkan REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

target_link_libraries(VoxelOps PRIVATE SDL3::SDL3)
target_link_libraries(VoxelOps PRIVATE glm::glm)
target_link_libraries(VoxelOps PRIVATE assimp::assimp)
target_link_libraries(VoxelOps PRIVATE glad::glad)
target_link_libraries(VoxelOps PRIVATE GameNetworkingSockets::shared GameNetworkingSockets::static GameNetworkingSockets::GameNetworkingSockets GameNetworkingSockets::GameNetworkingSockets_s)
target_link_libraries(VoxelOps PRIVATE Shared)
target_link_libraries(VoxelOps PRIVATE lz4::lz4)
target_link_libraries(VoxelOps PRIVATE imgui::imgui)
target_link_libraries(VoxelOps PRIVATE VoxelOps::SkyAtmosphereRenderer)
target_link_libraries(VoxelOps PRIVATE Vulkan::Vulkan)
target_link_libraries(VoxelOps PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(VoxelOps PRIVATE GLM_ENABLE_EXPERIMENTAL)
if(MSVC)
    set_property(TARGET VoxelOps PROPERTY VS_DEBUGGER_ENVIRONMENT "VOXELOPS_RENDER_API=vulkan")
endif()

target_include_directories(VoxelOps PRIVATE ${Stb_INCLUDE_DIR})
target_include_directories(VoxelOps PRIVATE ../Shared)

target_include_directories(VoxelOps PRIVATE ${VCPKG_INSTALLED_DIR}/include)
target_include_directories(VoxelOps PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

file(TO_CMAKE_PATH "${VOXELOPS_NRD_DLL_PATH}" VOXELOPS_NRD_DLL_PATH_CMAKE)
if(VOXELOPS_ENABLE_NRD_BOOTSTRAP AND EXISTS "${VOXELOPS_NRD_ROOT}/Include/NRD.h")
    message(STATUS "VoxelOps: NRD bootstrap enabled (root='${VOXELOPS_NRD_ROOT}')")
    target_include_directories(VoxelOps PRIVATE "${VOXELOPS_NRD_ROOT}/Include")
    target_compile_definitions(VoxelOps PRIVATE
        VOXELOPS_NRD_HEADERS=1
        VOXELOPS_NRD_DLL_PATH="${VOXELOPS_NRD_DLL_PATH_CMAKE}"
    )
else()
    message(STATUS "VoxelOps: NRD bootstrap disabled (set VOXELOPS_ENABLE_NRD_BOOTSTRAP=ON and VOXELOPS_NRD_ROOT to enable)")
    target_compile_definitions(VoxelOps PRIVATE VOXELOPS_NRD_HEADERS=0)
endif()

find_program(GLSLC_EXECUTABLE glslc REQUIRED)

set(VKSHADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/graphics/Vulkan/shaders/triangle.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/graphics/Vulkan/shaders/triangle.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/graphics/Vulkan/shaders/model.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/graphics/Vulkan/shaders/model.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/graphics/Vulkan/shaders/gi_probe_integrate.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/graphics/Vulkan/shaders/restir_gi_spatial.comp
)

set(SPIRV_OUTPUTS)
set(SPIRV_DIR "${CMAKE_BINARY_DIR}/shaders/spir-v")
file(MAKE_DIRECTORY ${SPIRV_DIR})

foreach(VKSHADER ${VKSHADERS})
    get_filename_component(FILE_NAME ${VKSHADER} NAME)
    get_filename_component(VKSHADER_ABS ${VKSHADER} ABSOLUTE)
    set(SPIRV "${SPIRV_DIR}/${FILE_NAME}.spv")

    add_custom_command(
        OUTPUT ${SPIRV}
        COMMAND ${GLSLC_EXECUTABLE} ${VKSHADER_ABS} -o ${SPIRV}
        DEPENDS ${VKSHADER_ABS}
        COMMENT "Compiling shader ${FILE_NAME}"
        VERBATIM
    )

    list(APPEND SPIRV_OUTPUTS ${SPIRV})
endforeach()

set(VKSHADERS_RT
    ${CMAKE_CURRENT_SOURCE_DIR}/graphics/Vulkan/shaders/triangle.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/graphics/Vulkan/shaders/model.frag
)

foreach(VKSHADER_RT ${VKSHADERS_RT})
    get_filename_component(FILE_NAME ${VKSHADER_RT} NAME)
    get_filename_component(VKSHADER_ABS ${VKSHADER_RT} ABSOLUTE)
    set(RT_FILE_NAME ${FILE_NAME})
    string(REPLACE ".frag" "_rt.frag" RT_FILE_NAME ${RT_FILE_NAME})
    set(SPIRV_RT "${SPIRV_DIR}/${RT_FILE_NAME}.spv")

    add_custom_command(
        OUTPUT ${SPIRV_RT}
        COMMAND ${GLSLC_EXECUTABLE} --target-env=vulkan1.2 -DVOXELOPS_RAY_QUERY=1 ${VKSHADER_ABS} -o ${SPIRV_RT}
        DEPENDS ${VKSHADER_ABS}
        COMMENT "Compiling RT shader ${RT_FILE_NAME}"
        VERBATIM
    )

    list(APPEND SPIRV_OUTPUTS ${SPIRV_RT})
endforeach()

add_custom_target(VulkanShaders ALL DEPENDS ${SPIRV_OUTPUTS})
add_dependencies(VoxelOps VulkanShaders)
target_compile_definitions(VoxelOps PRIVATE SHADER_DIR="${SPIRV_DIR}/")



set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT "VoxelOps")
set_property(TARGET VoxelOps PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
set_property(TARGET VoxelOps PROPERTY INTERPROCEDURAL_OPTIMIZATION_DISTRIBUTION TRUE)

set(CMAKE_CONFIGURATION_TYPES "Debug;Release;Distribution" CACHE STRING "" FORCE)

if(NOT MSVC)
    target_compile_options(VoxelOps PRIVATE -fno-rtti -fno-exceptions)
endif()
