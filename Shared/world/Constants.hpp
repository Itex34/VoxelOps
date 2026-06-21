#pragma once


constexpr int CHUNK_SIZE = 16; //blocks
constexpr int CHUNK_VOLUME = CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE;


//--IN CHUNKS--
constexpr int WORLD_MIN_X = -20;
constexpr int WORLD_MAX_X = 20;

constexpr int WORLD_MIN_Z = -20;
constexpr int WORLD_MAX_Z = 20;

//--IN BLOCKS--
constexpr int WORLD_MIN_Y = -16; // bedrock layer
constexpr int WORLD_MAX_Y = 64;

//--IN CHUNKS--
constexpr int WORLD_SIZE_X = WORLD_MAX_X - WORLD_MIN_X + 1;
constexpr int WORLD_SIZE_Z = WORLD_MAX_Z - WORLD_MIN_Z + 1;