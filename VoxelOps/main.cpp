#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "graphics/Model.hpp"
#include "graphics/Shader.hpp"
#include "graphics/Camera.hpp"
#include "graphics/ChunkMeshBuilder.hpp"
#include "voxels/Chunk.hpp"
#include "graphics/Frustum.hpp"
#include "player/Player.hpp"
#include "input/InputCallbacks.hpp"
#include "data/GameData.hpp"
#include "graphics/TextureAtlas.hpp"
#include "graphics/ChunkManager.hpp"
#include "physics/RayManager.hpp"
#include "physics/Raycast.hpp"
#include "network/ClientNetwork.hpp"
#include "physics/Physics.hpp"
#include "graphics/Renderer.hpp"

#include <sstream>
#include <optional>

#include <iostream>
#include <fstream>
#include <filesystem>





float windowWidth = 640;
float windowHeight = 480;

Camera debugCamera(glm::vec3(0.0f, 100.0f, 0.0f));


bool useDebugCamera = false;



static void updateFPSCounter(GLFWwindow* window) {
    GameData::frameCount++;
    double currentTime = glfwGetTime();
    double elapsedTime = currentTime - GameData::fpsTime;

    if (elapsedTime >= 1.0f) {
        double fps = GameData::frameCount / elapsedTime;
        std::stringstream ss;
        ss << "Voxel Ops - FPS: " << fps;
        glfwSetWindowTitle(window, ss.str().c_str());

        GameData::frameCount = 0;
        GameData::fpsTime = currentTime;
    }
}

int main(int argc, char** argv) {
    if (!glfwInit()) return -1;

    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(GameData::screenWidth, GameData::screenHeight, "Voxel Ops", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return -1;
    }
    glfwMakeContextCurrent(window);
    gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
    //glfwSwapInterval(0);

    printf("OpenGL version: %s\n", glGetString(GL_VERSION));
    printf("GLSL version: %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));

    Renderer renderer;



    ChunkManager chunkManager(renderer);



    chunkManager.enableAO = true;
    chunkManager.enableShadows = true;


    chunkManager.generateInitialChunks_TwoPass(WORLD_MAX_X);
    chunkManager.debugMemoryEstimate();



    Player player(glm::vec3(0.0f, 60.0f, 0.0f), chunkManager, "C:/Users/Sophie/source/repos/VoxelOps/Models/sniper.fbx");

    InputCallbacks inputCallbacks(player);

    RayManager rayManager;
    ClientNetwork clientNet;



    glfwSetWindowUserPointer(window, &inputCallbacks);
    glfwSetFramebufferSizeCallback(window, [](GLFWwindow* w, int width, int height) {
        static_cast<InputCallbacks*>(glfwGetWindowUserPointer(w))->framebuffer_size_callback(w, width, height);
        });

    glfwSetCursorPosCallback(window, [](GLFWwindow* w, double x, double y) {
        static_cast<InputCallbacks*>(glfwGetWindowUserPointer(w))->mouse_callback(w, x, y, useDebugCamera);
        });

    glfwSetMouseButtonCallback(window, [](GLFWwindow* w, int button, int action, int mods) {
        static_cast<InputCallbacks*>(glfwGetWindowUserPointer(w))->mouse_button_callback(w, button, action, mods);
        });
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);




    Shader chunkShader(
        "../../../../VoxelOps/shaders/allLightingPack.vert",
        "../../../../VoxelOps/shaders/allLightingPack.frag"
    );


    Shader dbgShader(
        "../../../../VoxelOps/shaders/debugVert.vert", 
        "../../../../VoxelOps/shaders/debugFrag.frag"
    );

    Shader skyShader(
        "../../../../VoxelOps/shaders/sky.vert",
        "../../../../VoxelOps/shaders/sky.frag"
    );




    Frustum frustum;

    glEnable(GL_DEPTH_TEST);
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    // frustum debug lines
    GLuint lineVAO, lineVBO;
    glGenVertexArrays(1, &lineVAO);
    glGenBuffers(1, &lineVBO);
    glBindVertexArray(lineVAO);
    glBindBuffer(GL_ARRAY_BUFFER, lineVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 3 * 24, nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);


    unsigned int skyVAO, skyVBO;
    {
        float skyVerts[] = {
            -1.0f, -1.0f,  
             3.0f, -1.0f,  
            -1.0f,  3.0f   
        };
        glGenVertexArrays(1, &skyVAO);
        glGenBuffers(1, &skyVBO);
        glBindVertexArray(skyVAO);
        glBindBuffer(GL_ARRAY_BUFFER, skyVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(skyVerts), skyVerts, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
        glBindVertexArray(0);
    }

    static const glm::vec3 ndcCorners[8] = {
        {-1, -1, -1}, {1, -1, -1}, {-1, 1, -1}, {1, 1, -1}, // near
        {-1, -1,  1}, {1, -1,  1}, {-1, 1,  1}, {1, 1,  1}  // far
    };

    static const int edges[12][2] = {
        {0,1},{1,3},{3,2},{2,0},
        {4,5},{5,7},{7,6},{6,4},
        {0,4},{1,5},{2,6},{3,7}
    };



    double lastX = 0.0f, lastY = 0.0f;
    float yaw = 0;
    float pitch = 0;

    double xoffset;
    double yoffset;

    double xpos, ypos;

    bool toggleWireframe = false;
    bool toggleChunkBorders = false;
    bool toggleDebugFrustum = false;

    bool wasF1Pressed = false;
    bool wasTPressed = false;
    bool wasF2Pressed = false;
    bool wasF3Pressed = false;

    glm::ivec3 lastChunkPos = glm::ivec3(INT_MAX);




    uint32_t netSeq = 0;
    double lastNetSendTime = glfwGetTime();
    const double netSendInterval = 0.1; // seconds -> 10 Hz

    // networking setup 
    if (!clientNet.Start()) {
        std::cerr << "Failed to start networking\n";
    }
    else {
        // connect to server (loopback or real IP)
        const char serverIp[] = "127.0.0.1";
        const uint16_t serverPort = 27015; // change to your server port
        if (!clientNet.ConnectTo(serverIp, serverPort)) {
            std::cerr << "ConnectTo(" << serverIp << ":" << serverPort << ") failed\n";
        }
        else {
            // send registration/username once. Keep it short <= server max.
            clientNet.SendConnectRequest("player1");
        }
    }


    bool setSkyUniforms = false;
    bool setUniforms = false;




    while (!glfwWindowShouldClose(window)) {
        glfwGetCursorPos(window, &xpos, &ypos);


        glm::vec3 moveDir(0.0f);

        if (glfwGetKey(window, GLFW_KEY_U) == GLFW_PRESS)
            moveDir += debugCamera.XZfront; // forward
        if (glfwGetKey(window, GLFW_KEY_J) == GLFW_PRESS)
            moveDir -= debugCamera.XZfront; // backward
        if (glfwGetKey(window, GLFW_KEY_H) == GLFW_PRESS)
            moveDir -= glm::normalize(glm::cross(debugCamera.front, debugCamera.up)); // left
        if (glfwGetKey(window, GLFW_KEY_K) == GLFW_PRESS)
            moveDir += glm::normalize(glm::cross(debugCamera.front, debugCamera.up)); // right
        if (glfwGetKey(window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS)
            moveDir += debugCamera.up; // up
        if (glfwGetKey(window, GLFW_KEY_V) == GLFW_PRESS)
            moveDir -= debugCamera.up; // down

        if (glm::length(moveDir) > 0.0f)
            moveDir = glm::normalize(moveDir);

        float currentSpeed = 10;

        debugCamera.position += moveDir * currentSpeed * (float)GameData::deltaTime;




        if(useDebugCamera) {
            xoffset = xpos - lastX;
            yoffset = ypos - lastY;

            lastX = xpos;
            lastY = ypos;

            xoffset *= 0.1;
            yoffset *= 0.1;

            yaw += xoffset;
            pitch -= yoffset;
            pitch = glm::clamp(pitch, -89.0f, 89.0f);
        }



        debugCamera.updateRotation(yaw, pitch);




        bool isF1Pressed = glfwGetKey(window, GLFW_KEY_F1) == GLFW_PRESS;

        if (isF1Pressed && !wasF1Pressed) {
            useDebugCamera = !useDebugCamera;
        }

        wasF1Pressed = isF1Pressed;



        bool T_IsPressed = glfwGetKey(window, GLFW_KEY_T) == GLFW_PRESS;

        if (T_IsPressed && !wasTPressed) {
            toggleWireframe = !toggleWireframe;
        }

        wasTPressed = T_IsPressed;

        const Camera& activeCamera = useDebugCamera ? debugCamera : player.getCamera();


        inputCallbacks.processInput(window);
        player.update(window, GameData::deltaTime);

        //glClearColor(0.4667f, 0.7098f, 0.9961f, 1.0f);

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glm::mat4 skyProjection = glm::perspective(glm::radians(GameData::FOV), (float)GameData::screenWidth / (float)GameData::screenHeight, 0.1f, 100000.0f);
        glm::mat4 skyView = activeCamera.getViewMatrix();

        // --- draw sky (fullscreen triangle) ---
        glDepthMask(GL_FALSE);          // don't write depth
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE); // optional: ensures sky always draws (you can keep depth test on if you prefer)
        skyShader.use();

        // inverse matrices for ray reconstruction
        glm::mat4 invProj = glm::inverse(skyProjection);
        glm::mat4 invView = glm::inverse(skyView);
        skyShader.setMat4("uInvProj", invProj);
        skyShader.setMat4("uInvView", invView);

        // --- sun / atmosphere ---
        glm::vec3 sunDir = glm::normalize(glm::vec3(1.0f, 1.0f, 0.15f));

        float groundOffset = 0.0f; // terrain height if you have it
        glm::vec3 camPos = player.getCamera().position; // your camera pos vec3



        skyShader.setVec3("uCameraPos", camPos);




        if(!setSkyUniforms)
        {
            skyShader.setVec3("uSunDir", sunDir);

            skyShader.setFloat("uGroundOffset", groundOffset);

            skyShader.setFloat("uSunIntensity", 1.1f); // if shader expects this name
            skyShader.setFloat("uSunRadiance", 1.1f);  // if shader expects this name

            // Atmosphere parameters (cover both "-Strength" and "-Scale" variants)
            skyShader.setFloat("uTurbidity", 0.1f);
            skyShader.setFloat("uMieG", 0.76f);
            skyShader.setFloat("uRayleighStrength", 2.0f);
            skyShader.setFloat("uRayleighScale", 2.0f); // duplicate-friendly
            skyShader.setFloat("uMieStrength", 0.01f);
            skyShader.setFloat("uMieScale", 0.01f);     // duplicate-friendly

            // Tone & gamma
            skyShader.setFloat("uExposure", 0.45f);
            skyShader.setFloat("uGamma", 2.2f);

            // Horizon / zenith helpers (different names across shaders)
            skyShader.setFloat("uZenithBoost", 0.0f);
            skyShader.setFloat("uHorizonBoost", 0.01f); // small aesthetic boost near horizon

            // Flat-ground shader has uGroundY sometimes
            skyShader.setFloat("uGroundY", groundOffset);

            // Flat-ground shader has uGroundY sometimes
            skyShader.setFloat("uGroundY", groundOffset);

            // Debug / mode switches (safe defaults)
            skyShader.setInt("uDebugMode", 0);


            setSkyUniforms = true;
        }





        // draw fullscreen triangle
        glBindVertexArray(skyVAO);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);

        //glEnable(GL_CULL_FACE);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);


        glm::mat4 projection = glm::perspective(glm::radians(GameData::FOV), (float)GameData::screenWidth / (float)GameData::screenHeight, 0.1f, 100000.0f);
        glm::mat4 view = activeCamera.getViewMatrix();
        glm::mat4 playerCamView = player.getCamera().getViewMatrix();
        glm::mat4 viewProjection = projection * view;

        glm::mat4 playerCamViewProjection = projection * playerCamView;
        frustum.extractPlanes(playerCamViewProjection);


        glm::vec3 lightDir = glm::normalize(glm::vec3(1.0f, 0.5f, 0.0f));
        glm::vec3 lightColor = glm::vec3(1.0f, 0.97f, 0.94f); // mild warm sun

        glm::vec3 ambientColor = glm::vec3(0.3f, 0.3f, 0.3f);


        //player.render(projection, lightDir, lightColor, ambientColor);


        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, chunkManager.atlas.atlasTextureID);

        chunkShader.use();


        chunkShader.setMat4("viewProj", viewProjection);






        if (!setUniforms) {
            chunkShader.setVec3("lightDir", lightDir);
            chunkShader.setVec3("lightColor", lightColor);
            if (chunkManager.enableAO) {


                // hemisphere — gentler blue, less influence
                chunkShader.setVec3("skyColorTop", glm::vec3(0.66f, 0.78f, 0.92f));
                chunkShader.setVec3("skyColorBottom", glm::vec3(0.96f, 0.92f, 0.84f));

                // strengths
                chunkShader.setFloat("ambientStrength", 0.80f);   // lower ambient so textures show through
                chunkShader.setFloat("diffuseStrength", 0.85f);   // slightly stronger direct light
                chunkShader.setFloat("minAmbient", 0.01f);        // allow darker crevices without flat grey


                // tint / post-processing
                chunkShader.setFloat("hemiTint", 0.5f);    // almost no hue tint from hemisphere
                chunkShader.setFloat("contrast", 1.0f);    // very gentle
                chunkShader.setFloat("satBoost", 1.17f);
                chunkShader.setVec3("warmth", glm::vec3(1.04f, 1.00f, 0.95f));

                chunkShader.setFloat("aoPow", 0.8f);
                chunkShader.setFloat("aoMin", 0.6f);
                chunkShader.setFloat("aoApplyAfterTone", 0.8f); // 0 = subtle, 1 = strong


                chunkShader.setFloat("shadowDarkness", 0.3f);
                chunkShader.setFloat("shadowContrast", 1.3f);

            }
            //lighting


            chunkShader.setVec3("cameraPos", player.getCamera().position);


            setUniforms = true;
        }

        chunkShader.setInt("texture1", 0);

        glPolygonMode(GL_FRONT_AND_BACK, toggleWireframe ? GL_LINE : GL_FILL); 

        chunkManager.renderChunks(chunkShader, frustum, player, player.renderDistance);
        

        glm::ivec3 currentChunk = chunkManager.worldToChunkPos(player.getPosition());





        bool F2_isPressed = glfwGetKey(window, GLFW_KEY_F2) == GLFW_PRESS;
        
        if (F2_isPressed && !wasF2Pressed) {
            toggleChunkBorders = !toggleChunkBorders;
        }

        wasF2Pressed = F2_isPressed;
        
        if (toggleChunkBorders) { 
            chunkManager.renderChunkBorders(view, projection);
        }





        bool F3_isPressed = glfwGetKey(window, GLFW_KEY_F3) == GLFW_PRESS;


        if (F3_isPressed && !wasF3Pressed) {
            toggleDebugFrustum = !toggleDebugFrustum;
        }

        wasF3Pressed = F3_isPressed;

        if (toggleDebugFrustum) {
            frustum.drawFrustumFaces(
                dbgShader,
                projection * player.getViewMatrix(),
                view,
                projection,
                toggleWireframe
            );

        }



        if (glfwGetKey(window, GLFW_KEY_H) == GLFW_PRESS) {

            Ray ray(player.getCamera().position, player.getCamera().front);

            if (rayManager.rayHasBlockIntersectSingle(ray, chunkManager, player.maxReach).hit) {
                chunkManager.playerBreakBlockAt(rayManager.rayHasBlockIntersectSingle(ray, chunkManager, player.maxReach).hitBlockWorld);
            }
        }


        if (glfwGetKey(window, GLFW_KEY_G) == GLFW_PRESS) {

            Ray ray(player.getCamera().position, player.getCamera().front);

            if (rayManager.rayHasBlockIntersectSingle(ray, chunkManager, player.maxReach + 100.05f /*so we can rebuild the block we just broke*/).hit) {
                chunkManager.playerPlaceBlockAt(rayManager.rayHasBlockIntersectSingle(ray, chunkManager, player.maxReach + 100.05f).hitBlockWorld, 0, BlockID::Dirt);
            }
        }


        // process networking incoming messages
        clientNet.Poll();

        // send position at fixed interval (10Hz)
        double now = glfwGetTime();
        if (now - lastNetSendTime >= netSendInterval) {
            lastNetSendTime = now;

            glm::vec3 pos = player.getPosition();
            glm::vec3 vel = glm::vec3(0);
            if (!clientNet.SendPosition(netSeq++, pos, vel)) {
                // optional: logging on failure, but avoid spamming
                // std::cerr << "SendPosition failed\n";
            }
        }


        updateFPSCounter(window);
        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    clientNet.Shutdown();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
