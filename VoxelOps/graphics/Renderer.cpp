


#include "Renderer.hpp"
#include "Mesh.hpp"

GLuint Renderer::loadTexture(const char* path) {
    int width, height, nrChannels;
    stbi_set_flip_vertically_on_load(true);
    unsigned char* data = stbi_load(path, &width, &height, &nrChannels, 0);
    if (!data) { std::cerr << "Failed to load texture: " << path << std::endl; return 0; }

    GLenum internalFormat, format;
    if (nrChannels == 1) {
        internalFormat = GL_R8;
        format = GL_RED;
    }
    else if (nrChannels == 3) {
        internalFormat = GL_SRGB8;      // sRGB internal for correct sampling -> linear data in shader
        format = GL_RGB;
    }
    else if (nrChannels == 4) {
        internalFormat = GL_SRGB8_ALPHA8;
        format = GL_RGBA;
    }
    else {
        std::cerr << "Unsupported channel count: " << nrChannels << std::endl;
        stbi_image_free(data);
        return 0;
    }

    GLuint textureID;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_2D, textureID);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);


    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, GL_UNSIGNED_BYTE, data);

    //glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

    GLfloat aniso = 2.0f;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &aniso);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, aniso);

    glEnable(GL_FRAMEBUFFER_SRGB);

    stbi_image_free(data);

    glBindTexture(GL_TEXTURE_2D, 0);
    return textureID;
}


void Renderer::initWorldBuffers()
{
    // ==========================
    // 1. VAO WITH WORLD GEOMETRY
    // ==========================
    glGenVertexArrays(1, &worldVAO);
    glBindVertexArray(worldVAO);

    // ---- VERTEX BUFFER ----
    glGenBuffers(1, &worldVBO);
    glBindBuffer(GL_ARRAY_BUFFER, worldVBO);
    glBufferData(GL_ARRAY_BUFFER,
        MAX_VERTEX_BUFFER_BYTES,
        nullptr,
        GL_STATIC_DRAW);

    // ---- INDEX BUFFER ----
    glGenBuffers(1, &worldEBO);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, worldEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
        MAX_INDEX_BUFFER_BYTES,
        nullptr,
        GL_STATIC_DRAW);

    // ---- VERTEX ATTRIBUTES ----
    glEnableVertexAttribArray(0);
    glVertexAttribIPointer(0, 1, GL_UNSIGNED_INT,
        sizeof(VoxelVertex),
        (void*)offsetof(VoxelVertex, low));

    glEnableVertexAttribArray(1);
    glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT,
        sizeof(VoxelVertex),
        (void*)offsetof(VoxelVertex, high));



    glBindVertexArray(0);
}








void Renderer::allocateMesh(size_t vertexCount, size_t indexCount,
    size_t& outVertexOffset, size_t& outIndexOffset)
{
    const size_t vertexBytesReq = vertexCount * sizeof(VoxelVertex);
    const size_t indexBytesReq = indexCount * sizeof(unsigned short);

    const size_t vertexOffsetBytes = currentVertexOffset * sizeof(VoxelVertex);
    const size_t indexOffsetBytes = currentIndexOffset * sizeof(unsigned short);

    //std::cout << "[allocateMesh] curV=" << currentVertexOffset
    //    << " curI=" << currentIndexOffset
    //    << " reqV=" << vertexCount << " (" << vertexBytesReq << " bytes)"
    //    << " reqI=" << indexCount << " (" << indexBytesReq << " bytes)"
    //    << " MAX_VERTEX_BUFFER_BYTES=" << MAX_VERTEX_BUFFER_BYTES
    //    << " MAX_INDEX_BUFFER_BYTES=" << MAX_INDEX_BUFFER_BYTES
    //    << std::endl;

    // Check overflow for vertex buffer
    if (vertexOffsetBytes + vertexBytesReq > (size_t)MAX_VERTEX_BUFFER_BYTES) {
        std::cerr << "[allocateMesh] ERROR: vertex buffer overflow. "
            << "required end = " << (vertexOffsetBytes + vertexBytesReq)
            << " > MAX_VERTEX_BUFFER_BYTES = " << MAX_VERTEX_BUFFER_BYTES << std::endl;
        // fail fast: do not allocate; set out offsets to invalid/sentinel
        outVertexOffset = SIZE_MAX;
        outIndexOffset = SIZE_MAX;
        return;
    }

    // Check overflow for index buffer
    if (indexOffsetBytes + indexBytesReq > (size_t)MAX_INDEX_BUFFER_BYTES) {
        std::cerr << "[allocateMesh] ERROR: index buffer overflow. "
            << "required end = " << (indexOffsetBytes + indexBytesReq)
            << " > MAX_INDEX_BUFFER_BYTES = " << MAX_INDEX_BUFFER_BYTES << std::endl;
        outVertexOffset = SIZE_MAX;
        outIndexOffset = SIZE_MAX;
        return;
    }

    // All good, allocate
    outVertexOffset = currentVertexOffset;
    outIndexOffset = currentIndexOffset;

    currentVertexOffset += vertexCount;
    currentIndexOffset += indexCount;

}

