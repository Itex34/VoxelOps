# Vulkan GI Clipmap Plan

This file defines the first-pass probe clipmap layout and per-frame update budget.
Code counterpart: `GiClipmapPlan.hpp`.

## Cascade Layout

All cascades are player-centered and snapped to the cascade probe spacing.

1. Cascade 0 (near)
- Spacing: `2` blocks
- Grid: `24 x 12 x 24` probes
- Coverage: `48 x 24 x 48` blocks

2. Cascade 1 (mid)
- Spacing: `4` blocks
- Grid: `20 x 10 x 20` probes
- Coverage: `80 x 40 x 80` blocks

3. Cascade 2 (far)
- Spacing: `8` blocks
- Grid: `16 x 8 x 16` probes
- Coverage: `128 x 64 x 128` blocks

## Probe Data Layout

- Irradiance texture tile: `6x6` interior + `1` texel border -> `8x8` tile per probe.
- Visibility texture tile: `14x14` interior + `1` texel border -> `16x16` tile per probe.
- Per-cascade texture packing:
- Width = `probeCountX * tileSize`
- Height = `(probeCountY * probeCountZ) * tileSize`

## Update Budget

1. Cascade 0
- Update cadence: every frame
- Max probes per update: `256`
- Rays per probe: `128`
- Worst-case rays this cascade in one frame: `32,768`

2. Cascade 1
- Update cadence: every `3` frames
- Max probes per update: `160`
- Rays per probe: `64`
- Burst rays when scheduled: `10,240`
- Average rays per frame contribution: `3,413`

3. Cascade 2
- Update cadence: every `8` frames
- Max probes per update: `96`
- Rays per probe: `32`
- Burst rays when scheduled: `3,072`
- Average rays per frame contribution: `384`

## Aggregate Cost Envelope

- Worst-case burst (if all cascades update same frame): `46,080` rays/frame.
- Long-run average with current cadence: about `36,565` rays/frame.

## Scheduling Strategy

- Each cascade owns a round-robin update cursor in flattened probe-index space.
- On a scheduled frame, each cascade updates up to `maxProbeUpdatesPerTick` probes.
- Next frame continues from prior cursor position.
- This keeps per-frame work stable and avoids full-volume spikes.

## Immediate Integration Target

1. Create per-cascade runtime state:
- snapped origin in blocks
- update cursor

2. At frame start:
- snap cascade origins from player position
- generate `GiFrameBudget` from `buildFrameBudget(...)`

3. Drive GI compute dispatch from `GiFrameBudget`:
- dispatch only scheduled probe ranges for each cascade
- keep GI resolve running every frame
