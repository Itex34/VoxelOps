#include "gi_tracing_rt.glsl"

TraceResult traceScene(vec3 origin, vec3 direction, float maxDistance) {
    return traceSceneRt(origin, direction, maxDistance);
}

bool traceSceneVisibility(vec3 origin, vec3 direction, float maxDistance) {
    return traceSceneRtVisibility(origin, direction, maxDistance);
}

bool traceSceneVisibilityNormalized(vec3 origin, vec3 directionNormalized, float maxDistance) {
    return traceSceneRtVisibilityNormalized(origin, directionNormalized, maxDistance);
}
