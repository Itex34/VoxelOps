## Prerequisites
- Windows
- CMake + Ninja
- MSVC toolchain
- vcpkg dependencies installed (toolchain path is set in `CMakePresets.json`)

## Build (Release)
```powershell
cmake --preset x64-release
cmake --build out/build/x64-release-ninja --config Release
```

## Run Locally
From repo root:
```powershell
.\start_server.bat
.\start_client.bat
```

## Host For Friends
1. Start server on host machine:
```powershell
.\start_server.bat 27015
```
2. Forward UDP `27015` on your router to the host machine.
3. Share your public IP with friends.
4. Friend joins:
```powershell
.\start_client.bat <host-public-ip> 27015 friend_name
```

## Client CLI
```text
--server-ip <host>    default: 127.0.0.1
--server-port <port>  default: 27015
--name <username>     optional (max 32 chars)
--help
```

## Server CLI
```text
--port <port>         default: 27015
--help
```

## Vulkan ray-stage profiling

For a repeatable local comparison, start a local server, then run a stationary bot with
one diagnostic ablation enabled:

```powershell
$env:VOXELOPS_PROFILE_STATIONARY_BOT = '1'
$env:VOXELOPS_PROFILE_RT_AB = 'primary_sun' # primary_sun, indirect, secondary_sun, second_bounce
$env:VOXELOPS_BENCHMARK = '1'
.\out\build\x64-release-ninja\VoxelOps\VoxelOps.exe --bot --bot-visible --bot-duration 25 --bot-seed 7 *> out\rt-profile.log
node tools\analyze-rt-profile.mjs out\rt-profile.log 2500 300
Remove-Item Env:VOXELOPS_PROFILE_RT_AB, Env:VOXELOPS_PROFILE_STATIONARY_BOT, Env:VOXELOPS_BENCHMARK
```

The profiler alternates normal and diagnostic shaders, so the displayed lighting and
denoiser history are intentionally modified during the run. Never use this mode for
visual-quality comparisons. Run each stage separately and choose a minimum frame after
the scene has loaded and the stationary bot has settled. The analyzer reports paired
terrain-pass timings for the last requested number of adjacent frame pairs. Ablations
also remove associated shading work and can change GPU scheduling, so the measured
differences are scene-specific estimates, not additive per-ray timings. Unsetting the
profiling variable restores the normal shader.

Experimental BLAS upgrades can be measured with `VOXELOPS_RT_BLAS_UPGRADES=1`.
They are disabled by default pending evidence that they improve traversal without
causing frame-time spikes. When enabled, the renderer only considers active,
unchanged BLASes of at most 512 triangles after a quiet period with frame-time
headroom. Upgrades yield to ordinary streaming; the previous BLAS remains in use
until the replacement and a new TLAS are ready. `VOXELOPS_VK_TIMING_LOG=1` logs
`rtFastTraceBlas` and the number of `upgrades` in sampled BLAS build batches.

## Notes
- Runtime asset/model/shared paths are resolved at startup, so launching from repo root or build folders works.
- Server admin data is written to `admins.txt` in the process working directory.
