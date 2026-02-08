#pragma once
#include "Chunk.hpp"

#include <map>


struct ChunkColumn {

	int8_t sunLitBlocksYvalue[16][16];// [+x -> -x][+z -> -z]


	int chunkX, chunkZ;//world position in chunks

	Chunk* subChunks[3]; //pointers to the 3 chunks in this collumn


	ChunkColumn() {
		for (int x = 0; x < 16; ++x) {

			for( int z = 0; z < 16; ++z){
				sunLitBlocksYvalue[x][z] = -17;


			}
		}

		for (int i = 0; i < 3; ++i) {
			subChunks[i] = nullptr;
		}
	}
};