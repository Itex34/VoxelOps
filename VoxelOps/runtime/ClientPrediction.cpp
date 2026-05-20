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
        GrappleConstraintState grappleConstraint{};
        grappleConstraint.active = runtime.combat.grapple.isAttached;
        grappleConstraint.anchor = runtime.combat.grapple.anchorPoint;
        grappleConstraint.ropeLength = runtime.combat.grapple.ropeLength;
        runtime.gameplay.player->simulateFromNetworkInput(
            intent.networkInput,
            deltaTime,
            true,
            runtime.combat.grapple.isAttached,
            &grappleConstraint
        );
    }
}
