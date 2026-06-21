cmake_minimum_required(VERSION 3.28 FATAL_ERROR)

project(VoxelOps CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

option(VOXELOPS_OPENGL_ONLY "Build VoxelOps with OpenGL backend only" OFF)

option(VOXELOPS_ENABLE_NRD_BOOTSTRAP "Enable early NRD API bootstrap integration" ON)
set(VOXELOPS_NRD_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../NRD" CACHE PATH "Path to NVIDIA NRD repository")
set(VOXELOPS_NRD_DLL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../NRD-Sample/_Bin/NRD.dll" CACHE FILEPATH "Path to NRD.dll used by runtime bootstrap")

add_executable(VoxelOps
    "main.cpp"  
    "graphics/Camera.cpp" 
    "graphics/OpenGL/Shader.cpp" 
    "render/meshing/ChunkMeshBuilder.cpp"
    "voxels/Voxel.cpp"
    "voxels/Chunk.cpp" 
    "graphics/OpenGL/Renderer.cpp"
    "graphics/OpenGL/OpenGLChunkScene.cpp"
    "graphics/OpenGL/OpenGLChunkRenderCache.cpp"
    "graphics/OpenGL/OpenGLGunRenderer.cpp"
    "graphics/OpenGL/OpenGLGunSceneRenderer.cpp"
    "graphics/OpenGL/OpenGLMesh.cpp"
    "graphics/OpenGL/OpenGLModel.cpp"
    "graphics/OpenGL/OpenGLRemotePlayerRenderer.cpp"
    "graphics/OpenGL/OpenGLRenderDevice.cpp"
    "graphics/OpenGL/OpenGLTextureAtlas.cpp"
    "graphics/OpenGL/passes/OpenGLSkyPass.cpp"
    "graphics/OpenGL/passes/OpenGLWorldPass.cpp"
    "graphics/VulkanRenderDevice.cpp"
    "graphics/RenderDeviceFactory.cpp"
    "graphics/ModelGeometry.cpp"
    "graphics/Vulkan/renderer/FrameSync.cpp"
    "graphics/Vulkan/renderer/NrdBootstrap.cpp"
    "graphics/Vulkan/renderer/Pipeline.cpp"
    "graphics/Vulkan/renderer/RenderPass.cpp"
    "graphics/Vulkan/renderer/VulkanRenderer.cpp"
    "graphics/Vulkan/renderer/VulkanRendererFrame.cpp"
    "graphics/Vulkan/renderer/VulkanGiHistory.cpp"
    "graphics/Vulkan/renderer/VulkanCommandRecording.cpp"
    "graphics/Vulkan/renderer/VulkanGiSignalClear.cpp"
    "graphics/Vulkan/renderer/VulkanGiResources.cpp"
    "graphics/Vulkan/renderer/VulkanSwapchainResources.cpp"
    "graphics/Vulkan/graphics/Mesh.cpp"
    "graphics/Vulkan/graphics/Model.cpp"
    "graphics/Vulkan/graphics/StbImageImpl.cpp"
    "graphics/Vulkan/graphics/VkTexture.cpp"
    "graphics/Vulkan/VulkanSceneUploader.cpp"
    "graphics/Vulkan/VulkanFrameBuilder.cpp"
    "graphics/Vulkan/VulkanGiSettings.cpp"
    "graphics/Vulkan/VulkanGiSceneBuffers.cpp"
    "graphics/Vulkan/VulkanRayTracingScene.cpp"
    "graphics/Vulkan/VulkanChunkRenderCache.cpp"
    "graphics/Vulkan/vulkan/UploadContext.cpp"
    "graphics/Vulkan/vulkan/VulkanContext.cpp"
    
    "graphics/Vulkan/vulkan/VulkanUtils.cpp"
    "graphics/Vulkan/renderer/VulkanRendererTiming.cpp"
    "graphics/Vulkan/renderer/VulkanDrawResources.cpp"
    "graphics/Vulkan/renderer/VulkanNrdResources.cpp"

    "player/Player.cpp"
    "player/PlayerMovement.cpp"
    "input/InputCallbacks.cpp"
    "data/GameData.hpp"
    "data/GameData.cpp" 
    "graphics/Frustum.hpp"
    "world/ChunkManager.cpp"
    "world/ChunkStreamingClient.cpp"
    "render/meshing/ChunkMesher.cpp"
    "graphics/AtlasLayout.cpp"
    "physics/Raycast.cpp" 
    "physics/RayManager.cpp"
    "application/AppWindow.cpp"
    "application/AppSetup.cpp"
    
    "ui/Hud.cpp"
    "graphics/OpenGL/WorldItemRenderer.cpp"
    "application/AppSimulation.cpp"
    "application/AppInputEvents.cpp"
    "application/AppLifecycle.cpp"
    "application/AppHelpers.cpp"
    "application/FrameServices.cpp"
    "application/ClientInputSystem.cpp"
    "application/ClientDisconnectReset.cpp"
    "application/ClientSession.cpp"
    "application/FrameOrchestrator.cpp"
    "render/RenderSceneBuilder.cpp"
    "systems/WorldInteractionSystem.cpp"
    "systems/CombatShootSystem.cpp"
    "systems/CombatGrappleSystem.cpp"
    
    "graphics/Lighting.cpp"
    "network/ClientNetwork.cpp"
    "network/ClientConnection.cpp"
    "network/ClientNetworkSend.cpp"
    "network/ClientNetworkReceive.cpp"
    "network/ClientNetworkQueues.cpp"
    "network/ClientPacketParsing.cpp"
    "network/ClientMessageParsing.cpp"
    "network/DecompressChunk.cpp"
    "runtime/ClientReconciler.cpp"
    "runtime/SnapshotInterpolator.cpp"
    "player/Hitbox.cpp"    
    "gun/Gun.cpp"
    "misc/ThreadPool.cpp" 
    "graphics/OpenGL/RegionMeshBuffer.cpp"
    "voxels/ChunkColumn.cpp"
    "graphics/OpenGL/Backend.cpp"
    "graphics/OpenGL/Sky.cpp"
    "graphics/ISkyBackend.hpp"
    "graphics/OpenGL/ShaderSkyBackend.hpp"
    "graphics/OpenGL/ShaderSkyBackend.cpp"
    
    "ui/debug/DebugUi.cpp"
    "ui/rml/RmlUiSystem.cpp"
    "ui/player/InventoryUI.cpp"
    "ui/player/Crosshair.cpp"
    "ui/player/UiStateController.cpp"
    "commands/CommandParser.cpp"
    "ui/player/SettingsUI.cpp"
    "ui/player/MainMenu.cpp"
    "runtime/ClientPrediction.cpp" 
    "graphics/Vulkan/vulkan/VmaImpl.cpp" "graphics/OpenGL/debug_render/RenderLine.cpp" "ui/widgets/UIContext.cpp" "ui/text/FontManager.cpp" "ui/text/Font.cpp" "ui/text/FontAtlasBuilder.cpp" "render/ITextureFactory.cpp")

find_package(SDL3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(assimp CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(GameNetworkingSockets CONFIG REQUIRED)
find_package(lz4 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(RmlUi REQUIRED)

find_package(Vulkan REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Freetype CONFIG REQUIRED)


target_link_libraries(VoxelOps PRIVATE SDL3::SDL3)

target_link_libraries(VoxelOps PRIVATE glm::glm)
target_link_libraries(VoxelOps PRIVATE assimp::assimp)
target_link_libraries(VoxelOps PRIVATE glad::glad)
target_link_libraries(VoxelOps PRIVATE GameNetworkingSockets::GameNetworkingSockets)
target_link_libraries(VoxelOps PRIVATE Shared)
target_link_libraries(VoxelOps PRIVATE lz4::lz4)
target_link_libraries(VoxelOps PRIVATE imgui::imgui)
target_link_libraries(VoxelOps PRIVATE RmlUi::RmlUi)
target_link_libraries(VoxelOps PRIVATE Freetype::Freetype)


target_link_libraries(VoxelOps PRIVATE Vulkan::Vulkan)
target_link_libraries(VoxelOps PRIVATE nlohmann_json::nlohmann_json)
target_link_libraries(VoxelOps PRIVATE GPUOpen::VulkanMemoryAllocator)


target_include_directories(VoxelOps PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/VkFFT/vkFFT
)


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




set(SPIRV_OUTPUTS)
set(SPIRV_DIR "${CMAKE_BINARY_DIR}/shaders/spir-v")

if(NOT VOXELOPS_OPENGL_ONLY)
    find_program(GLSLC_EXECUTABLE glslc REQUIRED)

    set(VKSHADERS
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/Vulkan/shaders/VoxelTerrainGI.vert
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/Vulkan/shaders/VoxelTerrainGI.frag
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/Vulkan/shaders/model.vert
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/Vulkan/shaders/model.frag
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/Vulkan/shaders/nrd_composite.vert
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/Vulkan/shaders/nrd_composite.frag
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/Vulkan/shaders/postprocess.vert
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/Vulkan/shaders/postprocess.frag
    )

    set(VK_SHADER_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/graphics/Vulkan/shaders")
    file(MAKE_DIRECTORY ${SPIRV_DIR})

    foreach(VKSHADER ${VKSHADERS})
        get_filename_component(FILE_NAME ${VKSHADER} NAME)
        get_filename_component(VKSHADER_ABS ${VKSHADER} ABSOLUTE)
        set(SPIRV "${SPIRV_DIR}/${FILE_NAME}.spv")

        add_custom_command(
            OUTPUT ${SPIRV}
            COMMAND ${GLSLC_EXECUTABLE} -I ${VK_SHADER_INCLUDE_DIR} ${VKSHADER_ABS} -o ${SPIRV}
            DEPENDS ${VKSHADER_ABS}
            COMMENT "Compiling shader ${FILE_NAME}"
            VERBATIM
        )

        list(APPEND SPIRV_OUTPUTS ${SPIRV})
    endforeach()

    set(VKSHADERS_RT
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/Vulkan/shaders/VoxelTerrainGI.frag
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
            COMMAND ${GLSLC_EXECUTABLE} --target-env=vulkan1.2 -I ${VK_SHADER_INCLUDE_DIR} -DVOXELOPS_RAY_QUERY=1 ${VKSHADER_ABS} -o ${SPIRV_RT}
            DEPENDS ${VKSHADER_ABS}
            COMMENT "Compiling RT shader ${RT_FILE_NAME}"
            VERBATIM
        )



        list(APPEND SPIRV_OUTPUTS ${SPIRV_RT})
    endforeach()
    add_custom_target(VulkanShaders ALL DEPENDS ${SPIRV_OUTPUTS})
    add_dependencies(VoxelOps VulkanShaders)
    target_compile_definitions(VoxelOps PRIVATE VOXELOPS_VULKAN_SHADERS_BUILT=1)
else()
    target_compile_definitions(VoxelOps PRIVATE VOXELOPS_VULKAN_SHADERS_BUILT=0)
endif()

target_compile_definitions(VoxelOps PRIVATE SHADER_DIR="${SPIRV_DIR}/")



set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT "VoxelOps")
set_property(TARGET VoxelOps PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
set_property(TARGET VoxelOps PROPERTY INTERPROCEDURAL_OPTIMIZATION_DISTRIBUTION TRUE)

set(CMAKE_CONFIGURATION_TYPES "Debug;Release;Distribution" CACHE STRING "" FORCE)

if(NOT MSVC)
    target_compile_options(VoxelOps PRIVATE -fno-rtti -fno-exceptions)
endif()
