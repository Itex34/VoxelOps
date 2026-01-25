#pragma once
#include <glad/glad.h>
#include <iostream>


#include "Model.hpp"
#include "Mesh.hpp"


constexpr int FACE_VERTICES = 6; // two triangles
constexpr int FACE_INDICES = 6; // two triangles

constexpr int MAX_FACES_PER_CHUNK = 4096; // worst case : one face for every voxel

constexpr int MAX_VERTICES_PER_CHUNK = MAX_FACES_PER_CHUNK * FACE_VERTICES;
constexpr int MAX_INDICES_PER_CHUNK = MAX_FACES_PER_CHUNK * FACE_INDICES;

constexpr int MAX_CHUNKS_LOADED = 1024;

constexpr int MAX_VERTEX_BUFFER_BYTES = 256 * 1024 * 1024;



//MAX_VERTICES_PER_CHUNK * MAX_CHUNKS_LOADED * 8;

constexpr int MAX_INDEX_BUFFER_BYTES = 128 * 1024 * 1024;


 
// MAX_INDICES_PER_CHUNK * MAX_CHUNKS_LOADED * sizeof(unsigned short);

struct VoxelVertex;






struct GpuMeshStats {
	size_t totalVertexCapacity;
	size_t totalIndexCapacity;

	size_t usedVertexCount;
	size_t usedIndexCount;

	size_t freeVertexCount;
	size_t freeIndexCount;

	size_t largestFreeVertexBlock;
	size_t largestFreeIndexBlock;
};


struct BufferRange;
struct ChunkMesh;


class Renderer {
public:
	Renderer() = default;

	GLuint loadTexture(const char* path);

	void beginFrame();
	void endFrame();

	void drawMesh(const ChunkMesh& mesh);

private:
	// Renderer does NOT own VAOs anymore
};



struct DrawElementsIndirectCommand {
	GLuint  count;
	GLuint  instanceCount;
	GLuint  firstIndex;
	GLuint  baseVertex;
	GLuint  baseInstance;
};