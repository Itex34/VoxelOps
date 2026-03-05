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
--server-ip <ipv4>    default: 127.0.0.1
--server-port <port>  default: 27015
--name <username>     optional (max 32 chars)
--help
```

## Server CLI
```text
--port <port>         default: 27015
--help
```

## Notes
- Runtime asset/model/shared paths are resolved at startup, so launching from repo root or build folders works.
- Server admin data is written to `admins.txt` in the process working directory.
