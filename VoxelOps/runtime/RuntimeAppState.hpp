#pragma once

#include "../runtime/RuntimeConnectionState.hpp"
#include "../runtime/RuntimeInputState.hpp"
#include "../runtime/RuntimePerfState.hpp"

struct RuntimeAppState {
    RuntimePerfState perf;
    RuntimeInputState inputLook;
    RuntimeConnectionState connection;
};
