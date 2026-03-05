@echo off
setlocal

set "ROOT=%~dp0"
set "EXE=%ROOT%out\build\x64-release-ninja\VoxelOps-Headless\VoxelOps-Headless.exe"

if not exist "%EXE%" (
  echo [start_server] Missing executable:
  echo   %EXE%
  echo Build first:
  echo   cmake --preset x64-release
  echo   cmake --build out/build/x64-release-ninja --config Release
  exit /b 1
)

set "PORT=27015"
if not "%~1"=="" set "PORT=%~1"

pushd "%ROOT%"
"%EXE%" --port %PORT%
set "EXITCODE=%ERRORLEVEL%"
popd

exit /b %EXITCODE%
