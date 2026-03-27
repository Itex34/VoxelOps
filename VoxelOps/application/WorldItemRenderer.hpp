#pragma once

class Camera;
struct Runtime;

class WorldItemRenderer {
public:
    void render(const Runtime& runtime, const Camera& activeCamera);
    void shutdown();

private:
    void ensureCubeMesh();

    unsigned int m_worldItemVao = 0;
    unsigned int m_worldItemVbo = 0;
};
