#pragma once	
#include <cmath>

namespace Shared::Utils {


    inline float NormalizeYawDegrees(float yawDegrees) {
        if (!std::isfinite(yawDegrees)) {
            return 0.0f;
        }
        float y = std::fmod(yawDegrees, 360.0f);
        if (y >= 180.0f) y -= 360.0f;
        if (y < -180.0f) y += 360.0f;
        return y;
    }
}
