// #include "VoxelopsApp.hpp"
//
//
// App::App() {
//     debugCamera.emplace(glm::vec3(0.0f, 100.0f, 0.0f));
//     player.emplace(glm::vec3(0.0f, 0.20f, 0.0f), physics);
//     inputCallbacks.emplace(player);
//
//     shader.emplace(
//         "../../../../VoxelOps/shaders/vertex.vert",
//         "../../../../VoxelOps/shaders/fragment.frag"
//     );
//
//     dbgShader.emplace(
//         "../../../../VoxelOps/shaders/debugVert.vert",
//         "../../../../VoxelOps/shaders/debugFrag.frag"
//     );
//
//     window = SDL_CreateWindow(GameData::screenWidth, GameData::screenHeight, "Voxel Ops",
//     nullptr, nullptr);
// }
//
// void App::updateFPSCounter(SDL_Window* window) {
//     GameData::frameCount++;
//     double currentTime = SDL_GetTicksNS();
//     double elapsedTime = currentTime - GameData::fpsTime;
//
//     if (elapsedTime >= 1.0f) {
//         double fps = GameData::frameCount / elapsedTime;
//         std::stringstream ss;
//         ss << "Voxel Ops - FPS: " << fps;
//         SDL_SetWindowTitle(window, ss.str().c_str());
//
//         GameData::frameCount = 0;
//         GameData::fpsTime = currentTime;
//     }
// }
//
// void App::Run() {
//
// }
//
// void App::renderDebug() {
//     bool F2_isPressed = SDL_GetKeyboardState(window, SDL_SCANCODE_F2) == true;
//
//     if (F2_isPressed && !wasF2Pressed) {
//         toggleChunkBorders = !toggleChunkBorders;
//     }
//
//     wasF2Pressed = F2_isPressed;
//
//     if (toggleChunkBorders) {
//         chunkManager.renderChunkBorders(view, projection);
//     }
//
//
//
//
//
//     bool F3_isPressed = SDL_GetKeyboardState(window, SDL_SCANCODE_F3) == true;
//
//
//     if (F3_isPressed && !wasF3Pressed) {
//         toggleDebugFrustum = !toggleDebugFrustum;
//     }
//
//     wasF3Pressed = F3_isPressed;
//
//     if (toggleDebugFrustum) {
//         frustum.drawFrustumFaces(
//             dbgShader,
//             projection * player.getViewMatrix(),
//             view,
//             projection,
//             toggleWireframe
//         );
//
//     }
// }
