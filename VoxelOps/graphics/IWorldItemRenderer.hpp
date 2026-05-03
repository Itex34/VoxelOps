#pragma once

class Camera;
struct Runtime;

class IWorldItemRenderer {
public:
    virtual ~IWorldItemRenderer() = default;

    virtual void render(const Runtime &runtime, const Camera &activeCamera) = 0;
    virtual void shutdown() = 0;
};
