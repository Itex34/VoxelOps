#pragma once

struct RuntimePerfState {
    float frameCpuMs = 0.0f;
    float inputMs = 0.0f;
    float networkMs = 0.0f;
    float predictionMs = 0.0f;
    float gameplayMs = 0.0f;
    float renderCpuMs = 0.0f;
    float presentMs = 0.0f;
    float chunkStreamingMs = 0.0f;
};
