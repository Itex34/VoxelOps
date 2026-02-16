#pragma once


#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp> 
#include "../voxels/Chunk.hpp"
#include "../voxels/ChunkColumn.hpp"

#include "ChunkMeshBuilder.hpp"
#include "Frustum.hpp"
#include "Camera.hpp"
#include "Mesh.hpp"
#include "TextureAtlas.hpp"
#include "RegionMeshBuffer.hpp"


#include "../ExternLibs/FastNoiseLite.h"
#include "../ExternLibs/skarupke/flat_hash_map.hpp"
#include "../ExternLibs/tsl/robin_hash.h"
#include "../ExternLibs/tsl/robin_map.h"
#include "../ExternLibs/robin-hood-hashing/robin_hood.h"
#include "../misc/ThreadPool.hpp"

#include <optional>
#include <random>
#include <thread>
#include <unordered_map>
#include <chrono>

//--IN CHUNKS--
constexpr int WORLD_MIN_X = -20; 
constexpr int WORLD_MAX_X = 20;

constexpr int WORLD_MIN_Z = -20;
constexpr int WORLD_MAX_Z = 20;


//--IN BLOCKS--
constexpr int WORLD_MIN_Y = -16; // bedrock layer
constexpr int WORLD_MAX_Y = 32; 


//--IN CHUNKS--
constexpr int WORLD_SIZE_X = WORLD_MAX_X - WORLD_MIN_X + 1;
constexpr int WORLD_SIZE_Z = WORLD_MAX_Z - WORLD_MIN_Z + 1;

//--IN BLOCKS--
constexpr int WORLD_SIZE_Y = (WORLD_MAX_Y - WORLD_MIN_Y + 1);






// Region size in chunks (e.g., 8x8x8 chunks per region)
constexpr int REGION_SIZE = 8;

// Bytes per region (tune based on your needs)
constexpr size_t REGION_VERTEX_BYTES = 3 * 1024 * 1024; // 16 MB
constexpr size_t REGION_INDEX_BYTES = 2 * 1024 * 1024; // 8 MB


struct Vec3Hasher {
    size_t operator()(const glm::ivec3& v) const {
        return std::hash<int>()(v.x) ^ std::hash<int>()(v.y << 1) ^ std::hash<int>()(v.z << 2);
    }
};




struct IVec3Hash {
    std::size_t operator()(glm::ivec3 const& v) const noexcept {
        // mix the three 32-bit ints into a 64-bit value then reduce to size_t
        // using large primes for simple hashing (good enough for chunk coords)
        uint64_t x = static_cast<uint32_t>(v.x);
        uint64_t y = static_cast<uint32_t>(v.y);
        uint64_t z = static_cast<uint32_t>(v.z);
        uint64_t h = (x * 73856093u) ^ (y * 19349663u) ^ (z * 83492791u);
        return static_cast<std::size_t>(h);
    }
};


struct IVec2Hash {
    std::size_t operator()(glm::ivec2 const& v) const noexcept {
        // mix the three 32-bit ints into a 64-bit value then reduce to size_t
        // using large primes for simple hashing (good enough for chunk coords)
        uint64_t x = static_cast<uint32_t>(v.x);
        uint64_t y = static_cast<uint32_t>(v.y);
        uint64_t h = (x * 73856093u) ^ (y * 19349663u);
        return static_cast<std::size_t>(h);
    }
};




struct Region {
    glm::ivec3 regionPos;
    std::unique_ptr<RegionMeshBuffer> gpu;
    std::unordered_map<glm::ivec3, ChunkMesh, IVec3Hash> chunks;
    size_t vertexBytes = REGION_VERTEX_BYTES;
    size_t indexBytes = REGION_INDEX_BYTES;

    Region() = default;
    Region(glm::ivec3 pos, size_t vertexBytes, size_t indexBytes)
        : regionPos(pos)
        , vertexBytes(vertexBytes)
        , indexBytes(indexBytes)
        , gpu(std::make_unique<RegionMeshBuffer>(vertexBytes, indexBytes))
    {
    }
};





struct IVec3Eq {
    bool operator()(glm::ivec3 const& a, glm::ivec3 const& b) const noexcept {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
};






struct ChunkRange {
    uint32_t firstIndex;   // index offset in bigEBO (in indices)
    uint32_t indexCount;   // number of indices
    uint32_t baseVertex;   // base vertex index in bigVBO (in vertices)
    uint32_t vertexCount;  // current vertex count
    uint32_t vertexCapacity; // optional: reserved capacity for in-place updates
    uint32_t indexCapacity;  // optional: reserved capacity for in-place updates
    glm::ivec3 chunkPos;   // world chunk coordinates
    bool alive = true;
};


class Player;

class ChunkManager{
public:
    ChunkManager(Renderer& renderer_);
    void generateInitialChunks(int radiusChunks);
    void generateInitialChunks_TwoPass(int radiusChunks);

    void renderChunks(
        Shader& shader,
        Frustum& frustum,
        Player& player,
        int maxRenderDistance
    );

    void renderChunkBorders(glm::mat4& view, glm::mat4& projection);
    void setBlockInWorld(const glm::ivec3& worldPos, BlockID blockID);

    void setBlockGlobal(int worldX, int worldY, int worldZ, BlockID id);
    BlockID getBlockGlobal(int worldX, int worldY, int worldZ);

    void updateDirtyChunks();
    void updateChunks(const glm::ivec3& playerWorldPos, int renderDistance);
    void updateDirtyChunkAt(const glm::ivec3& chunkPos);

    void playerPlaceBlockAt(glm::ivec3 blockCoords, int faceNormal, BlockID blockType);
    void playerBreakBlockAt(const glm::ivec3& blockCoords);

    const std::unordered_map<glm::ivec3, Chunk, IVec3Hash>& getChunks() const {
        return chunkMap;
    }

    std::unordered_map<glm::ivec3, Chunk, IVec3Hash>& getChunks() {
        return chunkMap;
    }



    glm::ivec3 worldToChunkPos(const glm::ivec3& worldPos) const;
    glm::ivec3 worldToLocalPos(const glm::ivec3& worldPos) const;


    bool enableAO;
    bool enableShadows;

    TextureAtlas atlas;



    void debugMemoryEstimate();
private:

    std::unordered_map<glm::ivec3, Region, IVec3Hash> regions;


    std::unordered_map<glm::ivec3, Chunk, IVec3Hash> chunkMap;


    std::unordered_map<glm::ivec3, ChunkMesh, IVec3Hash> chunkMeshes; //deprecated




    std::unordered_map<glm::ivec2, ChunkColumn, IVec2Hash> chunkColumns;

     
    // Convert chunk position to region position
    inline glm::ivec3 chunkToRegionPos(const glm::ivec3& chunkPos) const {
        return glm::ivec3(
            floorDiv(chunkPos.x, REGION_SIZE),
            floorDiv(chunkPos.y, REGION_SIZE),
            floorDiv(chunkPos.z, REGION_SIZE)
        );
    }

    // Get or create region for a chunk
    Region& getOrCreateRegion(const glm::ivec3& chunkPos);

	bool rebuildRegion(const glm::ivec3& regionPos, size_t reserveVertices = 0, size_t reserveIndices = 0);

    // Upload mesh to appropriate region
    void uploadChunkMesh(const glm::ivec3& chunkPos,
        const std::vector<VoxelVertex>& vertices,
        const std::vector<uint16_t>& indices);

    // Remove mesh from region
    void removeChunkMesh(const glm::ivec3& chunkPos);


    
    void appendChunkMesh();

    void generateChunkAt(const glm::ivec3& pos);

    void generateTerrainChunkAt(const glm::ivec3& pos);

    void markChunkDirty(const glm::ivec3& pos);
    void requestChunkRebuild(const glm::ivec3& pos);
    void buildChunkMeshWorker(glm::ivec3 pos);

    void placeTree(Chunk& chunk, const glm::ivec3& basePos, std::mt19937& gen);
    bool inBounds(const glm::ivec3& pos) const;


    void setBlockSafe(Chunk& currentChunk, const glm::ivec3& pos, BlockID id);
    BlockID getBlockSafe(Chunk& currentChunk, const glm::ivec3& pos);




    void computeLowestPotentialOccluders(const glm::ivec3& chunkPos, const Chunk& chunk);

    void computeHeightMap(const glm::ivec3& columnPos, const ChunkColumn& col);
    void rebuildColumnSunCache(int colChunkX, int colChunkZ);
    void updateColumnSunCacheForBlockChange(int worldX, int worldY, int worldZ, BlockID oldId, BlockID newId);
    int getColumnTopOccluderY(int worldX, int worldZ) const;

    int16_t scanDown(int x, int startY, int z, ChunkColumn col);

    ChunkColumn& getOrCreateColumn(int colX, int colZ);



    ChunkMeshBuilder builder;
    std::array<bool, 6> getVisibleChunkFaces(const glm::ivec3& pos) const;

    GLuint wireVAO, wireVBO;

    std::optional<Shader> debugShader;



    glm::vec4 m_tileInfo[256]; 
    bool m_tileInfoInitialized = false;




    ThreadPool meshPool{ std::max(1u, std::thread::hardware_concurrency() - 1) };



    FastNoiseLite noise;



    inline std::array<bool, 6> isEdgeBlock(glm::ivec3 localPos){
        return {
            (localPos.x == 0), (localPos.x == CHUNK_SIZE - 1),
            (localPos.y == 0), (localPos.y == CHUNK_SIZE - 1),
            (localPos.z == 0), (localPos.z == CHUNK_SIZE - 1)
        };
    }

    inline std::array<bool, 8> isCornerBlock(glm::ivec3 localPos) {
        return {
            (localPos.x == 0 && localPos.y == 0), (localPos.x == (CHUNK_SIZE - 1) && localPos.y == (CHUNK_SIZE - 1)),
            (localPos.y == 0 && localPos.z == 0), (localPos.y == (CHUNK_SIZE - 1) && localPos.z == (CHUNK_SIZE - 1)),

            (localPos.x == 0 && localPos.y == (CHUNK_SIZE - 1)), (localPos.x == (CHUNK_SIZE - 1) && localPos.y == 0),
            (localPos.y == 0 && localPos.z == (CHUNK_SIZE - 1)), (localPos.y == (CHUNK_SIZE - 1) && localPos.z == 0),
        };
    }


    inline int floorDiv(int a, int b) const{
        int q = a / b;
        int r = a % b;
        if ((r != 0) && ((r > 0) != (b > 0))) {
            q--;
        }
        return q;
    }

    inline int mod(int a, int b) const {
        int r = a % b;
        if (r < 0) r += std::abs(b);
        return r;
    }

    Renderer& renderer;
};





