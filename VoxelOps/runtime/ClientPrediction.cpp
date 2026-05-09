#include "ClientPrediction.hpp"


#include "Runtime.hpp"
#include "../application/ClientInputIntent.hpp"


void ClientPrediction::update(
	Runtime& runtime,
	const ClientInputIntent& intent,
	double deltaTime
) {
    if (!runtime.gameplay.player) {
        return;
    }

    runtime.gameplay.player->setNetworkInputState(intent.networkInput);
    if (deltaTime > 0.0) {
        runtime.gameplay.player->simulateFromNetworkInput(intent.networkInput, deltaTime, true);
    }
}