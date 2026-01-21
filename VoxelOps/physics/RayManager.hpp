#pragma once

#include "Raycast.hpp"
#include "../graphics/ChunkManager.hpp"
#include "../player/Hitbox.hpp"

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


struct RayShootHit {
	bool hit;
	glm::vec3 hitPoint;
	float distance;
	enum class Type { None, Block, Player } type;
	glm::ivec3 blockPos;      // valid if Type::Block
	glm::ivec3 chunkPos;      // valid if Type::Block
	Player* player = nullptr; // valid if Type::Player
	HitRegion region;         // valid if Type::Player
};


class RayManager {
public:
	RayManager();

	RayResult rayHasBlockIntersectBatch(std::list<Ray>& rays);
	RayResult rayHasBlockIntersectSingle(const Ray& ray, const ChunkManager& chunkManager, float maxDistance); //for block breaking/placing


	//for shooting
	RayShootHit rayShoot(
		const glm::vec3& origin,
		const glm::vec3& dir,
		const ChunkManager& chunkManager,
		const std::vector<Player*>& players,
		float maxDistance
	);
private:
	//ChunkManager& chunkManager;

	glm::ivec3 chunkHitCoords;

};
