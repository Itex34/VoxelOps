#pragma once
#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "../physics/Physics.hpp"
#include "../graphics/Renderer.hpp"
#include "../graphics/Model.hpp"
#include "../graphics/Shader.hpp"
#include "../graphics/Camera.hpp"
#include "../graphics/ChunkMeshBuilder.hpp"
#include "../graphics/Frustum.hpp"
#include "../player/Player.hpp"
#include "../input/InputCallbacks.hpp"
#include "../data/GameData.hpp"
#include "../graphics/TextureAtlas.hpp"
#include "../graphics/ChunkManager.hpp"


#include <sstream>
#include <optional>


class Camera;
class Player;
class InputCallbacks;
class Shader;
class Physics;
class ChunkManager;


class App {
public:
	App(){}
	void Run(){}
	void Exit(){}


	void updateFPSCounter(GLFWwindow* window);
private:
	float windowWidth = 640;
	float windowHeight = 480;


	bool useDebugCamera = false;

	void renderDebug();



	Camera debugCamera;
	Player player;
	InputCallbacks inputCallbacks;
	Shader shader;
	Shader dbgShader;

	Physics physics;
	ChunkManager chunkManager;
	//TextureAtlas atlas;
	Frustum frustum;





	GLFWwindow* window;



	// ---DEBUG VARS---


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
};