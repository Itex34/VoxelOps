#pragma once

#include "../network/ClientNetwork.hpp"
#include "../runtime/ClientReconciler.hpp"
#include "../runtime/SnapshotInterpolator.hpp"

struct RuntimeNetworkState {
    ClientNetwork clientNet;
    SnapshotInterpolator snapshotInterpolator;
    ClientReconciler reconciler;
};
 