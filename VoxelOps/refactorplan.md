# Refactor Plan (Post A-F Status)

## Current Status
Phases A through F are functionally complete:
1. Backend coupling was reduced across `application/runtime/systems`.
2. Shared `RenderScene` contract is now the backend input boundary.
3. Vulkan renderer/device were split into smaller modules.
4. OpenGL legacy `ChunkRenderSystem` and old depth-path hooks were removed.
5. World/render boundary improved (`ChunkManager` moved out of `graphics/`, client worldgen removed).
6. Probe/clipmap GI paths were purged (PTGI + DDA/Hardware RT path switching remains).

## Verified in Phase F
1. No probe/clipmap GI symbols remain in client Vulkan C++ modules.
2. No probe/clipmap symbols remain in client shader tree.
3. Removed/retired probe-era module names are no longer referenced.

## Remaining Work (Actual)

### 1) Final world/render ownership cleanup
Objective: finish folder ownership so simulation-adjacent meshing is not owned by `graphics/`.
1. Move `graphics/ChunkMesher.*` to a non-backend module (for example `render/meshing` or `world/meshing`).
2. Keep `ChunkMeshBuilder` ownership explicit (domain meshing vs backend upload responsibilities).
3. Ensure `world/ChunkManager` depends only on neutral meshing interfaces, not backend namespace placement.

Exit criteria:
1. No simulation-owned meshing modules under backend-facing `graphics/`.
2. `ChunkManager` integration remains unchanged behaviorally.

### 2) Backend selection isolation
Objective: confine OpenGL/Vulkan branching to allowed seams.
1. Keep backend construction branching in `RenderDeviceFactory`.
2. Remove remaining backend selection branching from `application` where feasible by delegating policy to graphics layer/factory inputs.

Exit criteria:
1. `application/player/world/network/runtime` do not branch on backend type.
2. Only factory/backend-owned debug UI branch on backend specifics.

### 3) Final dead code pruning pass
Objective: reduce maintenance surface after all extractions.
1. Remove stale toggles/config that no longer affect active render paths.
2. Remove unused helpers introduced during migration splits once confirmed unreferenced.
3. Trim comments/log strings that still refer to legacy behavior where no longer applicable.

Exit criteria:
1. No stale config path that does not impact runtime behavior.
2. No dead helper modules left from extraction scaffolding.

## Definition of Done
1. Strict dependency direction remains: `application -> systems/orchestration -> domain modules -> low-level utilities`.
2. Backends consume shared scene contract without gameplay/network object leaks.
3. OpenGL and Vulkan backends are independently maintainable without cross-layer coupling.
4. No probe-GI runtime path remains in the client.
