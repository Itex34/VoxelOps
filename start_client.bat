@echo off
setlocal

set "ROOT=%~dp0"
set "EXE=%ROOT%out\build\x64-release-ninja\VoxelOps\VoxelOps.exe"

if not exist "%EXE%" (
  echo [start_client] Missing executable:
  echo   %EXE%
  echo Build first:
  echo   cmake --preset x64-release
  echo   cmake --build out/build/x64-release-ninja --config Release
  exit /b 1
)

set "SERVER_IP=127.0.0.1"
set "SERVER_PORT=27015"
set "PLAYER_NAME="

if not "%~1"=="" set "SERVER_IP=%~1"
if not "%~2"=="" set "SERVER_PORT=%~2"
if not "%~3"=="" set "PLAYER_NAME=%~3"

pushd "%ROOT%"
if defined PLAYER_NAME (
  "%EXE%" --server-ip "%SERVER_IP%" --server-port %SERVER_PORT% --name "%PLAYER_NAME%"
) else (
  "%EXE%" --server-ip "%SERVER_IP%" --server-port %SERVER_PORT%
)
set "EXITCODE=%ERRORLEVEL%"
popd

exit /b %EXITCODE%
