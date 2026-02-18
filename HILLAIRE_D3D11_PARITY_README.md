# Hillaire Sky: Exact D3D11 LUT Parity Plan (VoxelOps OpenGL 4.3)

This document defines the exact remaining work to match the D3D11 Hillaire/Bruneton LUT pipeline used in `UnrealEngineSkyAtmosphere`, so VoxelOps gets the same sky realism (hue richness, sunset behavior, sun vicinity).

## Target Reference (Must Match)

- Main LUT generation: `UnrealEngineSkyAtmosphere/Application/RenderWithLuts.cpp`
- Sky/camera-volume shaders: `UnrealEngineSkyAtmosphere/Resources/RenderWithLuts.hlsl`
- LUT pass shaders: `UnrealEngineSkyAtmosphere/Resources/SkyAtmosphereTransmittanceLut.hlsl`
- LUT pass shaders: `UnrealEngineSkyAtmosphere/Resources/SkyAtmosphereDirectIrradianceLut.hlsl`
- LUT pass shaders: `UnrealEngineSkyAtmosphere/Resources/SkyAtmosphereSingleScatteringLut.hlsl`
- LUT pass shaders: `UnrealEngineSkyAtmosphere/Resources/SkyAtmosphereScatteringDensity.hlsl`
- LUT pass shaders: `UnrealEngineSkyAtmosphere/Resources/SkyAtmosphereIndirectIrradiance.hlsl`
- LUT pass shaders: `UnrealEngineSkyAtmosphere/Resources/SkyAtmosphereMultipleScattering.hlsl`
- Shared math and mappings: `UnrealEngineSkyAtmosphere/Resources/SkyAtmosphereBruneton.hlsl`
- Shared math and mappings: `UnrealEngineSkyAtmosphere/Resources/Bruneton17/functions.glsl`

## Non-Negotiable Rule

Do not use custom approximations for final parity if they differ from the D3D11 LUT chain. Replace with the exact pass sequence and shader logic from the files above.

## Current Gap Summary

- Current VoxelOps advanced path uses custom sky-view, camera-volume, and multi-scattering shaders that are not the full D3D11 iterative pass chain.
- A true D3D11-equivalent scattering-order loop is missing.
- Irradiance LUT generation and iterative density/indirect/multiple-scattering accumulation are missing as separate passes.
- Final sky composite path is not yet identical to `RenderWithLuts.hlsl`.

## Exact Pass Chain To Implement

Implement this exact order every LUT rebuild:

1. Transmittance LUT pass
2. Direct Irradiance LUT pass
3. Single Scattering 3D LUT pass
4. For each scattering order `n = 2..N` (use `N = 5` default): run `Scattering Density 3D` for order `n`
5. For each scattering order `n = 2..N`: run `Indirect Irradiance` using side constant `ScatteringOrder = n - 1`
6. For each scattering order `n = 2..N`: run `Multiple Scattering 3D` for order `n`
7. Camera Volumes pass (scattering/transmittance 3D volumes)
8. Final sky composition pass using LUTs and scene depth

## Required LUT Sizes and Formats

Use the same dimensions as reference:

- Transmittance: `256 x 64` (`RGBA16F` or `RGBA32F`)
- Irradiance: `64 x 16` (`RGBA16F` or `RGBA32F`)
- Scattering 3D: `R = 32`, `MU = 128`, `MU_S = 32`, `NU = 8`
- Scattering 3D width: `NU * MU_S`
- Scattering 3D height: `MU`
- Scattering 3D depth: `R`
- Camera scattering volume: `32 x 32 x 32`
- Camera transmittance volume: `32 x 32 x 32`

Use `RGBA32F` for transmittance/irradiance/scattering LUTs if precision mismatch remains visible.

## Required GL Infrastructure

- Full-screen triangle VS for all LUT passes.
- Layered rendering for 3D LUT/volume passes via geometry shader (`gl_Layer`) and `glDrawArraysInstanced`.
- A strict texture binding map per pass matching DX shader register intent.
- Side constant buffer field: `ScatteringOrder`
- Side constant buffer field: `UseSingleMieScatteringTexture`
- Side constant buffer field: `LuminanceFromRadiance`
- Match blend behavior: default blend off for most passes
- Match blend behavior: per-target equivalent of DX `Blend0Nop1Add`; if unavailable, split into two passes
- Match blend behavior: final sky composition uses premultiplied blending equivalent to D3D11 path

## VoxelOps Code Areas To Replace/Refactor

- `VoxelOps/graphics/Sky.cpp`
- Replace current advanced LUT flow with full pass graph listed above.
- Add missing irradiance/scattering resources and programs.
- Add iterative scattering-order loop (`2..N`) and side constants updates.
- `VoxelOps/graphics/Sky.hpp`
- Add handles for all missing programs/textures/FBO attachments and side-constant state.
- `VoxelOps/shaders/sky_hillaire_luts.frag`
- Replace/align with `RenderWithLuts.hlsl` behavior for depth-aware sky + aerial perspective composition.

## Pass-by-Pass Mapping (DX -> GL)

Transmittance:
- DX: `TransmittanceLutPS`
- GL target: `transmittance_lut_tex`
- Inputs: atmosphere constants only

Direct Irradiance:
- DX: `DirectIrradianceLutPS`
- GL targets: `delta_irradiance_tex` and accumulated `irradiance_tex`
- Inputs: transmittance LUT

Single Scattering:
- DX: `SingleScatteringLutPS` + layered GS
- GL targets: `delta_rayleigh_tex`, `delta_mie_tex`, and accumulated `scattering_tex`
- Inputs: transmittance LUT

Scattering Density (iterative):
- DX: `ScatteringDensityLutPS`
- GL target: `delta_scattering_density_tex`
- Inputs: transmittance, delta irradiance, delta rayleigh, delta mie, delta multiple

Indirect Irradiance (iterative):
- DX: `IndirectIrradianceLutPS`
- GL targets: `delta_irradiance_tex` and accumulated `irradiance_tex`
- Inputs: delta rayleigh, delta mie, delta multiple
- Side constants: `ScatteringOrder = n - 1`

Multiple Scattering (iterative):
- DX: `MultipleScatteringLutPS`
- GL targets: `delta_multiple_tex` and accumulated `scattering_tex`
- Inputs: transmittance LUT, delta scattering density
- Side constants: `ScatteringOrder = n`

Camera Volumes:
- DX: `RenderCameraVolumesPS`
- GL MRT layered targets: camera scattering volume + camera transmittance volume
- Inputs: transmittance, irradiance, scattering LUTs

Final Sky Composite:
- DX: `RenderWithLutsPS`
- GL target: backbuffer HDR
- Inputs: transmittance, irradiance, scattering LUTs, camera volumes, depth

## Validation Protocol (Required)

Validate each stage before tuning:

1. Visualize each LUT texture/volume slice directly.
2. Compare GL vs D3D11 for the same sun elevation and camera altitude.
3. Lock exposure/tone mapping to the same values during comparison.
4. Validate horizon hue and sun-near glow at low sun angles.
5. Validate aerial perspective coloration over distant terrain/voxels.

## Done Criteria (Parity)

All conditions must be true:

- Full D3D11 pass chain exists and runs in the same order.
- Scattering-order loop is implemented and defaulted to `N=5`.
- Final composite uses all LUTs/volumes like `RenderWithLuts.hlsl`.
- Sunset and sun-near coloration are visually close to D3D11 under matched camera/sun settings.
- No fallback custom approximation path is active in parity mode.

## Recommended Implementation Order

1. Implement all missing LUT resources/programs and pass scheduling.
2. Implement iterative `n=2..N` loop with side constants.
3. Hook camera-volume pass to LUT outputs.
4. Align final composite with `RenderWithLuts.hlsl`.
5. Add a debug UI toggle to display each LUT pass output.
6. Tune only after pass parity is complete.
