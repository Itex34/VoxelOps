#pragma once

#include "../runtime/Runtime.hpp"

class ClientDisconnectReset {
public:
    void apply(Runtime &runtime, bool *forceCursorEnabled) const;
};
