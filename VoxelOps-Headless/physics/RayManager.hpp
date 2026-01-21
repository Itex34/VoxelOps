#pragma once

#include "Raycast.hpp"
#include "../graphics/ChunkManager.hpp"


#include <glm/glm.hpp>
#include <glm/fwd.hpp>
#include <list>
#include <iostream>



struct RayResult {
	bool hit;
	glm::ivec3 hitBlockWorld;
	glm::ivec3 hitChunk;
	float distance;
};



class RayManager {
public:
	RayManager();

	RayResult rayHasBlockIntersectBatch(std::list<Ray>& rays);
	RayResult rayHasBlockIntersectSingle(const Ray& ray, const ChunkManager& chunkManager, float maxDistance); //for block breaking/placing

	RayResult rayHasBlockIntersectSinglePrecise(const Ray& ray, const ChunkManager& chunkManager, float maxDistance);//for shooting
private:
	//ChunkManager& chunkManager;

	glm::ivec3 chunkHitCoords;

};
