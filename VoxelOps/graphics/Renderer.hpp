#pragma once
#include <glad/glad.h>
#include <iostream>


#include "Model.hpp"


constexpr int FACE_VERTICES = 6; // two triangles
constexpr int FACE_INDICES = 6; // two triangles

constexpr int MAX_FACES_PER_CHUNK = 4096; // worst case : one face for every voxel

constexpr int MAX_VERTICES_PER_CHUNK = MAX_FACES_PER_CHUNK * FACE_VERTICES;
constexpr int MAX_INDICES_PER_CHUNK = MAX_FACES_PER_CHUNK * FACE_INDICES;

constexpr int MAX_CHUNKS_LOADED = 1024;

constexpr int MAX_VERTEX_BUFFER_BYTES =
MAX_VERTICES_PER_CHUNK * MAX_CHUNKS_LOADED * 8; //size of voxel vertex is 8

constexpr int MAX_INDEX_BUFFER_BYTES =
MAX_INDICES_PER_CHUNK * MAX_CHUNKS_LOADED * sizeof(unsigned short);

struct VoxelVertex;

class Renderer {
public:
	GLuint loadTexture(const char* path);
	void initWorldBuffers();

	void allocateMesh(
		size_t vertexCount, 
		size_t indexCount,
		size_t& outVertexOffset,
		size_t& outIndexOffset
	);

	GLuint worldVAO;
	GLuint worldVBO;
	GLuint worldEBO;

	GLuint indirectVBO;
	GLuint modelSSBO;
	GLuint instanceID_SSBO;

	GLuint chunkBasesSSBO;

	size_t currentVertexOffset = 0;
	size_t currentIndexOffset = 0;
private:



};	



struct DrawElementsIndirectCommand {
	GLuint  count;
	GLuint  instanceCount;
	GLuint  firstIndex;
	GLuint  baseVertex;
	GLuint  baseInstance;
};