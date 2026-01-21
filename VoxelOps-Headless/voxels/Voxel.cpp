// BlocksServer.cpp
// Server-side replacement for Voxel.cpp / getTexCoordsForFace()
// No glm, no TextureAtlas — just returns texture *names* for faces.

#include "Voxel.hpp"
#include <stdexcept>

/// Face indices should match your previous convention:
// 0 = +X (Right)
// 1 = -X (Left)
// 2 = +Y (Top)
// 3 = -Y (Bottom)
// 4 = +Z (Front)
// 5 = -Z (Back)

/// Return the name of the texture tile that the client would use for this face.
/// This keeps the same logical API (you can ask "what texture goes on this face?")
/// while avoiding any graphics types on the server.
std::string getTextureNameForFace(BlockID blockID, int face) {
    auto it = blockTypes.find(blockID);
    if (it == blockTypes.end()) {
        // Unknown block -> no texture
        return std::string();
    }

    const BlockTexture& tex = it->second.textures;

    switch (face) {
    case 0: // +X (Right)
    case 1: // -X (Left)
        return tex.RLSide;
    case 4: // +Z (Front)
    case 5: // -Z (Back)
        return tex.FBSide;
    case 2: // +Y (Top)
        return tex.top;
    case 3: // -Y (Bottom)
        return tex.bottom;
    default:
        // If someone passes an invalid face index, return empty string.
        return std::string();
    }
}

/// Optional small helpers that may be useful on the server side.

// Safe accessor that returns a texture name but falls back to a provided default.
std::string getTextureNameForFaceOr(BlockID blockID, int face, const std::string& fallback) {
    std::string name = getTextureNameForFace(blockID, face);
    if (name.empty()) return fallback;
    return name;
}

// Convert face index to a human-readable name (useful for logging)
std::string faceName(int face) {
    switch (face) {
    case 0: return "+X (Right)";
    case 1: return "-X (Left)";
    case 2: return "+Y (Top)";
    case 3: return "-Y (Bottom)";
    case 4: return "+Z (Front)";
    case 5: return "-Z (Back)";
    default: return "InvalidFace";
    }
}
